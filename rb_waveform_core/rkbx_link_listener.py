# Copyright (c) mrmilbe

"""Rekordbox Link OSC listener utilities.

This module wraps the ``python-osc`` server used by ``rkbx_link`` so the GUI can
subscribe to deck updates (time + ANLZ path) without re-implementing the OSC
plumbing each time.  Typical usage::

    from rkbx_link_listener import RekordboxLinkListener

    def on_event(event):
        print(f"Deck {event.deck}: t={event.time_seconds}, ANLZ={event.anlz_path}")

    listener = RekordboxLinkListener(on_event=on_event)
    listener.start()
    ...
    listener.stop()

The listener tracks per-deck state so every callback receives both the latest
playhead time and current ANLZ folder path, enabling downstream waveform
manipulation or lazy loading of Rekordbox assets.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from pathlib import Path
from queue import Empty, Queue
import math
import threading
import time
from typing import Callable, Dict, Iterable, Optional

from pythonosc import dispatcher, osc_server

DEBUG = False


@dataclass(frozen=True)
class DeckEvent:
    """Snapshot of a deck update coming from Rekordbox Link."""

    deck: int
    time_seconds: Optional[float]
    anlz_path: Optional[str]
    current_bpm: Optional[float]
    original_bpm: Optional[float]
    time_scale: Optional[float]
    source: str
    received_at: float
    playing: Optional[bool] = None
    rate: Optional[float] = None
    loop_start: Optional[float] = None
    loop_end: Optional[float] = None
    loop_active: Optional[bool] = None


def format_link_status(
    deck: int,
    time_seconds: Optional[float],
    anlz_path: Optional[str],
) -> str:
    """Format a human-readable status string for a deck."""
    time_text = "-" if time_seconds is None else f"{time_seconds:6.2f}s"
    if anlz_path:
        name = Path(anlz_path).name or str(anlz_path)
        return f"RB Link deck {deck}: t={time_text} | {name}"
    return f"RB Link deck {deck}: t={time_text} | (no ANLZ)"


@dataclass
class DeckState:
    """Mutable state for a single deck."""

    deck: int
    time_seconds: Optional[float] = None
    anlz_path: Optional[str] = None
    current_bpm: Optional[float] = None
    original_bpm: Optional[float] = None
    last_update: float = 0.0
    playing: Optional[bool] = None
    rate: Optional[float] = None
    loop_start: Optional[float] = None
    loop_end: Optional[float] = None
    loop_active: Optional[bool] = None

    def snapshot(self, source: str, when: Optional[float] = None) -> DeckEvent:
        return DeckEvent(
            deck=self.deck,
            time_seconds=self.time_seconds,
            anlz_path=self.anlz_path,
            current_bpm=self.current_bpm,
            original_bpm=self.original_bpm,
            time_scale=self._time_scale(),
            source=source,
            received_at=when if when is not None else time.time(),
            playing=self.playing,
            rate=self.rate,
            loop_start=self.loop_start,
            loop_end=self.loop_end,
            loop_active=self.loop_active,
        )

    def _time_scale(self) -> Optional[float]:
        if self.current_bpm and self.original_bpm and self.current_bpm > 0:
            return self.original_bpm / self.current_bpm
        return None


class RekordboxLinkListener:
    """OSC listener that aggregates Rekordbox Link deck data."""

    def __init__(
        self,
        ip: str = "127.0.0.1",
        port: int = 4460,
        decks: Iterable[int] = (0, 1, 2, 3),
        on_event: Optional[Callable[[DeckEvent], None]] = None,
    ) -> None:
        self.ip = ip
        self.port = port
        self.decks = tuple(decks)
        self.on_event = on_event

        self._dispatcher = dispatcher.Dispatcher()
        self._dispatcher.set_default_handler(self._handle_default)
        self._queue: "Queue[DeckEvent]" = Queue()
        self._state: Dict[int, DeckState] = {deck: DeckState(deck=deck) for deck in self.decks}

        for deck in self.decks:
            self._dispatcher.map(f"/time/{deck}", self._handle_time)
            self._dispatcher.map(f"/track/{deck}/anlz_path", self._handle_anlz_path)
            self._dispatcher.map(f"/bpm/{deck}/current", self._handle_bpm_current)
            self._dispatcher.map(f"/bpm/{deck}/original", self._handle_bpm_original)
            self._dispatcher.map(f"/playing/{deck}", self._handle_playing)
            self._dispatcher.map(f"/rate/{deck}", self._handle_rate)
            self._dispatcher.map(f"/loop/{deck}", self._handle_loop)

        self._server: Optional[osc_server.ThreadingOSCUDPServer] = None
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._state_lock = threading.RLock()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def start(self) -> None:
        """Start the OSC listener in a background thread."""

        with self._lock:
            if self._server is not None:
                return
            self._server = osc_server.ThreadingOSCUDPServer((self.ip, self.port), self._dispatcher)
            self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
            self._thread.start()

    def stop(self) -> None:
        """Stop the OSC listener and wait for the thread to finish."""

        with self._lock:
            server, thread = self._server, self._thread
            self._server = None
            self._thread = None
        # Handlers also acquire the state lock; joining while holding it deadlocks.
        if server is not None:
            server.shutdown()
            server.server_close()
        if thread is not None:
            thread.join(timeout=1.0)

    def __enter__(self) -> "RekordboxLinkListener":  # pragma: no cover - convenience wrapper
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:  # pragma: no cover - convenience wrapper
        self.stop()

    @property
    def connected(self):
        return self._server is not None and any(time.time() - s.last_update < 2.0 for s in self._state.values())

    def get_state(self, deck: int) -> DeckState:
        """Return a copy of the current state for ``deck``."""

        if deck not in self._state:
            raise ValueError(f"Deck {deck} is not tracked")
        state = self._state[deck]
        return replace(state)

    def get_event(self, timeout: Optional[float] = None) -> Optional[DeckEvent]:
        """Retrieve the next queued deck event (or ``None`` on timeout)."""

        try:
            return self._queue.get(timeout=timeout)
        except Empty:
            return None

    # ------------------------------------------------------------------
    # OSC handlers
    # ------------------------------------------------------------------
    @staticmethod
    def _finite_argument(args):
        if len(args) != 1:
            return None
        try:
            value = float(args[0])
        except (TypeError, ValueError, OverflowError):
            return None
        return value if math.isfinite(value) else None

    @staticmethod
    def _flag(value):
        if isinstance(value, (bool, int, float)) and value in (0, 1):
            return bool(value)
        return None

    def _update_state(self, deck, source, **values):
        if deck not in self._state:
            return
        with self._state_lock:
            state = self._state[deck]
            for key, value in values.items():
                setattr(state, key, value)
            state.last_update = time.time()
            self._emit(state, source=source)

    def _handle_time(self, address: str, *args: float) -> None:
        value = self._finite_argument(args)
        if value is not None:
            self._update_state(self._deck_from_address(address), "time", time_seconds=value)

    def _handle_anlz_path(self, address: str, *args: str) -> None:
        deck = self._deck_from_address(address)
        if deck not in self._state or len(args) != 1:
            return
        path = args[0]
        if path is not None and not isinstance(path, str):
            return
        normalized = str(self._normalize_anlz_path(Path(path))) if path else None
        with self._state_lock:
            changed = normalized != self._state[deck].anlz_path
            reset = dict(playing=None, rate=None, loop_start=None, loop_end=None,
                         loop_active=None, time_seconds=None) if changed or not normalized else {}
            self._update_state(deck, "anlz_path", anlz_path=normalized, **reset)

    def _handle_bpm_current(self, address: str, *args: float) -> None:
        value = self._finite_argument(args)
        if value is not None and 0 <= value <= 1000:
            self._update_state(self._deck_from_address(address), "bpm_current", current_bpm=value)

    def _handle_bpm_original(self, address: str, *args: float) -> None:
        value = self._finite_argument(args)
        if value is not None and 0 <= value <= 1000:
            self._update_state(self._deck_from_address(address), "bpm_original", original_bpm=value)

    def _handle_playing(self, address: str, *args) -> None:
        flag = self._flag(args[0]) if len(args) == 1 else None
        if flag is not None:
            self._update_state(self._deck_from_address(address), "playing", playing=flag)

    def _handle_rate(self, address: str, *args: float) -> None:
        value = self._finite_argument(args)
        if value is not None and -4 <= value <= 4:
            self._update_state(self._deck_from_address(address), "rate", rate=value)

    def _handle_loop(self, address: str, *args) -> None:
        """Receive authoritative live loop [start seconds, end seconds, enabled]."""
        if len(args) != 3:
            return
        start, end = self._finite_argument(args[:1]), self._finite_argument(args[1:2])
        enabled = self._flag(args[2])
        if start is None or end is None or enabled is None:
            return
        if enabled and not 0 <= start < end:
            return
        self._update_state(self._deck_from_address(address), "loop", loop_active=enabled,
                           loop_start=start if enabled else None, loop_end=end if enabled else None)

    def _handle_default(self, address: str, *args) -> None:
        # Ignore noisy topics by default; hook here if needed for debugging.
        return

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    def _emit(self, state: DeckState, source: str) -> None:
        event = state.snapshot(source=source)
        self._queue.put(event)
        if self.on_event is not None:
            try:
                self.on_event(event)
            except Exception:
                # Avoid killing the listener because of downstream errors.
                if DEBUG:
                    print("[RB Link] Listener callback failed", flush=True)
                pass

    @staticmethod
    def _deck_from_address(address: str) -> Optional[int]:
        try:
            parts = address.strip("/").split("/")
            return int(parts[1])
        except (IndexError, ValueError):
            return None

    @staticmethod
    def _normalize_anlz_path(path: Path) -> Path:
        if path.suffix.lower() == ".dat":
            return path.parent
        return path
