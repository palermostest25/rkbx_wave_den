"""Rekordbox's own deck state, and track loading, through Wave's bridge inside Rekordbox (macOS).

Rekordbox loads the bridge - an OpenSSL provider with no algorithms - when Wave starts it with
OPENSSL_CONF pointing at Wave's config. The bridge reads Rekordbox's engine on Rekordbox's own
main thread and shares it through a private file; native/mac_bridge/ holds its source and the
layout mirrored here. When Rekordbox was started some other way, the Accessibility reader takes over.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import mmap
import os
from pathlib import Path
from queue import Empty, Queue
import shutil
import struct
import subprocess
import sys
import threading
import time

from .rkbx_link_listener import DeckState

APP = Path('/Applications/rekordbox 7/rekordbox.app')
EXECUTABLE = APP/'Contents'/'MacOS'/'rekordbox'
MAGIC, VERSION = b'RKWBRDG1', 3
HEADER = struct.Struct('<8sIIIIIiQQ128s')
DECK = struct.Struct('<IidfffIqqiI1024s')
COMMAND = struct.Struct('<IIIIQII')       # all but the trailing check
REPLY = struct.Struct('<Ii120s')
DECKS_AT = HEADER.size
COMMAND_AT = DECKS_AT + 4*DECK.size
REPLY_AT = COMMAND_AT + COMMAND.size + 8
SIZE = REPLY_AT + REPLY.size
STARTING, LIVE, UNSUPPORTED, FAILED = range(4)
LOADED, PLAYING, LOOPING, MASTER_TEMPO, SYNC, SLIP, KEY_SYNC = 1, 2, 4, 8, 16, 32, 64
LOAD = 1
REQUESTED = 1
assert (HEADER.size, DECK.size, SIZE) == (176, 1080, 4664)


def bridge_dir():
    return Path.home()/'Library'/'Application Support'/'rkbx_wave'/'bridge'


def uptime_ns():
    return time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)


def fnv(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


@dataclass(frozen=True)
class BridgeDeck:
    loaded: bool
    playing: bool
    position: float           # seconds
    duration: float
    bpm: float
    original_bpm: float
    loop: tuple | None        # (start, end) seconds while a loop runs
    path: str
    master: bool = False      # Rekordbox's tempo master
    master_tempo: bool = False  # key lock
    sync: bool = False
    slip: bool = False
    key_sync: bool = False
    key_shift: int = 0        # semitones


@dataclass(frozen=True)
class Snapshot:
    pid: int
    status: int
    note: str
    ticks: int
    uptime_ns: int
    decks: tuple


def parse(buffer):
    """One consistent snapshot of the shared state, or None while the bridge is mid-write."""
    if len(buffer) < SIZE:
        return None
    for _ in range(8):
        magic, version, seq, pid, status, count, master, ticks, beat, note = HEADER.unpack_from(buffer, 0)
        if magic != MAGIC:
            return None
        if version != VERSION:
            # A Rekordbox started by an earlier Wave: never misread its layout.
            return Snapshot(pid, UNSUPPORTED, 'Rekordbox has an older Wave bridge loaded: quit it and press CONNECT',
                            0, 0, ())
        if seq & 1:
            continue
        decks = []
        for index in range(min(count, 4)):
            flags, total, position, bpm, original, _, _, loop_in, loop_out, shift, _, path = \
                DECK.unpack_from(buffer, DECKS_AT + index*DECK.size)
            loaded = bool(flags & LOADED)
            decks.append(BridgeDeck(loaded, bool(flags & PLAYING), position/1000, total/1000, bpm/100, original/100,
                (loop_in/1000, loop_out/1000) if flags & LOOPING else None,
                path.split(b'\0', 1)[0].decode('utf-8', 'replace') if loaded else '',
                loaded and master == index, bool(flags & MASTER_TEMPO), bool(flags & SYNC), bool(flags & SLIP),
                bool(flags & KEY_SYNC), shift if loaded else 0))
        if HEADER.unpack_from(buffer, 0)[2] == seq:
            return Snapshot(pid, status, note.split(b'\0', 1)[0].decode('utf-8', 'replace'), ticks, beat, tuple(decks))
    return None


class Bridge:
    """The bridge's shared file: read deck state, ask Rekordbox to load tracks."""
    def __init__(self, path=None):
        self.path = Path(path) if path else bridge_dir()/'state.bin'
        self.file = open(self.path, 'r+b')
        try:
            stat = os.fstat(self.file.fileno())
            # Only ever our own private file of the full size: a short one would fault on read.
            if stat.st_uid != os.getuid() or stat.st_mode & 0o077 or stat.st_size < SIZE:
                raise OSError('Wave bridge state file is not private or not complete')
            self.map = mmap.mmap(self.file.fileno(), SIZE)
        except Exception:
            self.file.close()
            raise
        self.lock = threading.Lock()

    def close(self):
        self.map.close()
        self.file.close()

    def snapshot(self):
        return parse(self.map)

    def request_load(self, deck, content_id, timeout=2.5):
        """Ask Rekordbox to load collection track ``content_id`` on 0-based ``deck``.
        Returns (accepted, message). Acceptance means Rekordbox took the request; the deck
        state confirms the load."""
        content_id = int(content_id)
        if not 0 <= content_id <= 0xffffffff or not 0 <= deck < 4:
            return False, 'Not a Rekordbox collection track'
        with self.lock:
            seq = (struct.unpack_from('<I', self.map, COMMAND_AT)[0] + 1) & 0xffffffff or 1
            body = COMMAND.pack(seq, LOAD, deck, content_id, uptime_ns(), 0, 0)
            self.map[COMMAND_AT:COMMAND_AT + COMMAND.size + 8] = body + struct.pack('<Q', fnv(body[:28]))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                answered, result, message = REPLY.unpack_from(self.map, REPLY_AT)
                if answered == seq:
                    return result == REQUESTED, message.split(b'\0', 1)[0].decode('utf-8', 'replace')
                time.sleep(.005)
        return False, 'Rekordbox did not answer; is it still running?'


def rekordbox_pid():
    import psutil
    for process in psutil.process_iter(['exe']):
        if process.info.get('exe') == str(EXECUTABLE):
            return process.pid
    return None


def bundled_dylib():
    base = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent.parent))
    return base/'rb_waveform_core'/'data'/'wave-bridge.dylib'


def install(source=None):
    """Put the bridge and its OpenSSL config where Rekordbox may read them; returns the config.
    The library is named by its hash: a running Rekordbox maps it, so it is never overwritten."""
    source = Path(source) if source else bundled_dylib()
    data = source.read_bytes()
    folder = bridge_dir()
    folder.mkdir(parents=True, exist_ok=True)
    os.chmod(folder, 0o700)
    library = folder/f'wave-bridge-{hashlib.sha256(data).hexdigest()[:16]}.dylib'
    if not library.exists():
        partial = library.with_suffix('.partial')
        shutil.copyfile(source, partial)
        os.chmod(partial, 0o700)
        partial.replace(library)
    config = folder/'openssl.cnf'
    text = ('openssl_conf = wave_init\n[wave_init]\nproviders = wave_providers\n'
            '[wave_providers]\ndefault = default_provider\nwave_bridge = wave_bridge_provider\n'
            '[default_provider]\nactivate = 1\n'
            f'[wave_bridge_provider]\nmodule = {library}\nactivate = 1\n')
    if not config.exists() or config.read_text() != text:
        partial = config.with_suffix('.partial')
        partial.write_text(text)
        partial.replace(config)
    return config


def launch(hidden=False):
    """Start Rekordbox with the bridge, hidden behind Wave if asked. Never touches a Rekordbox
    that is already running."""
    if rekordbox_pid():
        raise RuntimeError('Quit Rekordbox first; Wave then starts it with the bridge')
    if not APP.is_dir():
        raise RuntimeError(f'Rekordbox was not found at {APP}')
    config = install()
    subprocess.run(['/usr/bin/open', *(('-g', '-j') if hidden else ()), '-a', str(APP),
                    '--env', f'OPENSSL_CONF={config}', '--env', 'RKBX_WAVE_BRIDGE=1'], check=True, timeout=15)


def show():
    """Bring a hidden Rekordbox to the front, for its own screens and preferences."""
    subprocess.run(['/usr/bin/open', '-a', str(APP)], check=True, timeout=15)


def live(snapshot, pid, now=None):
    """The bridge in this Rekordbox is reading its engine right now."""
    now = uptime_ns() if now is None else now
    return bool(snapshot and pid and snapshot.pid == pid and snapshot.status == LIVE
                and now - snapshot.uptime_ns < 1_000_000_000)


class RekordboxBridgeReader:
    """Deck events from the bridge; the Accessibility reader stands in when there is no bridge."""
    RELAUNCH = 'Quit Rekordbox and press CONNECT in Wave for exact loops and touch loading'

    def __init__(self, deck_count=4, poll_hz=120, fallback=None):
        self.deck_count = deck_count
        self.interval = 1/poll_hz
        self.connected = False
        self.bridged = False
        self.status = 'Waiting for Rekordbox'
        self.bridge = None
        self.deck_paths = {}  # deck -> audio file Rekordbox's engine has loaded, while bridged
        self.deck_status = {}  # deck -> BridgeDeck: master, sync, key lock... while bridged
        self._fallback_factory = fallback
        self._fallback = None
        self._queue = Queue()
        self._stop_event = threading.Event()
        self._thread = None

    def start(self):
        if self._thread is None:
            self._stop_event.clear()
            self._thread = threading.Thread(target=self._run, name='rekordbox-bridge', daemon=True)
            self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=2.5)
            self._thread = None
        self._stop_fallback()
        self.connected = self.bridged = False

    def get_event(self, timeout=None):
        try:
            return self._queue.get(timeout=timeout)
        except Empty:
            pass
        return self._fallback.get_event(timeout=0) if self._fallback else None

    def load(self, deck, content_id):
        bridge = self.bridge
        if not (bridge and self.bridged):
            return False, self.RELAUNCH
        return bridge.request_load(deck, content_id)

    def _start_fallback(self):
        if self._fallback is None:
            if self._fallback_factory is None:
                from .mac_accessibility_reader import RekordboxAccessibilityReader
                self._fallback_factory = RekordboxAccessibilityReader
            self._fallback = self._fallback_factory(deck_count=self.deck_count)
            self._fallback.start()

    def _stop_fallback(self):
        if self._fallback:
            self._fallback.stop()
            self._fallback = None

    def _run(self):
        from .library import Libraries
        library = None
        states = {i: DeckState(i) for i in range(self.deck_count)}
        paths = {}
        pid, next_pid, last_tick = None, 0.0, None
        try:
            while not self._stop_event.is_set():
                started = time.monotonic()
                try:
                    if started >= next_pid:
                        next_pid = started + 1
                        pid = rekordbox_pid()
                    state_file = bridge_dir()/'state.bin'
                    if self.bridge and not self.bridged and state_file.exists() and \
                            state_file.stat().st_ino != os.fstat(self.bridge.file.fileno()).st_ino:
                        self.bridge.close()  # replaced on disk: follow the new one
                        self.bridge = None
                    if self.bridge is None and state_file.exists():
                        self.bridge = Bridge(state_file)
                    snapshot = self.bridge.snapshot() if self.bridge else None
                    if snapshot is None and self.bridged:
                        # Caught Rekordbox mid-write (its thread was pre-empted): read again, never disconnect.
                        self._stop_event.wait(.002)
                        continue
                    if (self.bridged and snapshot and pid and snapshot.pid == pid and snapshot.status == LIVE
                            and not live(snapshot, pid)):
                        # Rekordbox's main thread is busy (a heavy load, a dialog): its bridge is still
                        # there and resumes with it, so hold every deck as it is rather than start over.
                        self.status = 'REKORDBOX · busy'
                        self._stop_event.wait(.05)
                        continue
                    if not live(snapshot, pid):
                        if self.bridged:
                            for idx in range(self.deck_count):  # forget bridge decks before another source
                                paths.pop(idx, None)
                                states[idx] = DeckState(idx)
                                self._queue.put(states[idx].snapshot('anlz_path'))
                        self.bridged = False
                        self.deck_paths = {}
                        self.deck_status = {}
                        if pid and snapshot and snapshot.pid == pid and snapshot.status in (UNSUPPORTED, FAILED):
                            reason = snapshot.note
                        elif pid and snapshot and snapshot.pid == pid:
                            reason = 'Rekordbox is starting'
                        else:
                            reason = self.RELAUNCH
                        if pid and reason != 'Rekordbox is starting':
                            self._start_fallback()
                            self.connected = self._fallback.connected
                            self.status = f'{self._fallback.status} · {reason}'
                        else:
                            self._stop_fallback()
                            self.connected = False
                            self.status = reason if pid else 'Waiting for Rekordbox · press CONNECT to start it with Wave'
                        self._stop_event.wait(.25)
                        continue
                    if not self.bridged:
                        self._stop_fallback()
                        while not self._queue.empty():  # nothing the fallback said survives
                            self._queue.get_nowait()
                        self.bridged = True
                    self.connected = True
                    self.status = 'REKORDBOX · direct'
                    if snapshot.ticks == last_tick:
                        self._stop_event.wait(max(0, self.interval - (time.monotonic() - started)))
                        continue
                    last_tick = snapshot.ticks
                    self.deck_status = dict(enumerate(snapshot.decks))
                    # Rekordbox's tick, not this read, is when the values were true.
                    when = time.time() - max(0, uptime_ns() - snapshot.uptime_ns)/1e9
                    for idx, deck in enumerate(snapshot.decks[:self.deck_count]):
                        state = states[idx]
                        if deck.path != paths.get(idx):
                            paths[idx] = deck.path
                            self.deck_paths = {**self.deck_paths, idx: deck.path}
                            state.anlz_path = state.title = state.artist = state.key = state.artwork = None
                            missing = None
                            if deck.path:
                                if library is None:
                                    library = Libraries()
                                track = library.resolve_path(deck.path)
                                if track:
                                    state.anlz_path = str(track.analysis_folder)
                                    state.title, state.artist, state.key = track.title, track.artist, track.key
                                    state.artwork = str(track.artwork) if track.artwork else None
                                else:
                                    missing = f'{Path(deck.path).name} is not in the Rekordbox library or a mounted USB'
                            state.time_seconds = None
                            self._queue.put(state.snapshot('anlz_path'))
                            if missing:
                                self._queue.put(replace(state.snapshot('status'), error=missing))
                        if not deck.loaded:
                            continue
                        state.time_seconds = deck.position
                        state.current_bpm = deck.bpm or None
                        state.original_bpm = deck.original_bpm or None
                        # Rate is left to the timeline: at 100 samples a second it measures
                        # platter moves and brakes, which the tempo alone would hide.
                        state.playing = deck.playing
                        state.loop_active = deck.loop is not None
                        state.loop_start, state.loop_end = deck.loop or (None, None)
                        state.last_update = when
                        self._queue.put(state.snapshot('bridge', when=when))
                except Exception as exc:
                    self.connected = self.bridged = False
                    self.status = f'Rekordbox bridge: {exc}'
                    if self.bridge:
                        self.bridge.close()
                        self.bridge = None
                    paths.clear()
                    self._stop_event.wait(.5)
                self._stop_event.wait(max(0, self.interval - (time.monotonic() - started)))
        finally:
            if library:
                library.close()
            if self.bridge:
                self.bridge.close()
                self.bridge = None
            self.connected = self.bridged = False
