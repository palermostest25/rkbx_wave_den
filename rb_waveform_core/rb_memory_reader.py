# Copyright (c) mrmilbe

"""Direct Rekordbox memory reader — drop-in replacement for RekordboxLinkListener.

Reads BPM, playhead position, and ANLZ path from Rekordbox's process memory via
ReadProcessMemory (using pymem), removing the rkbx_link subprocess and OSC hop.

Pointer offsets are version-keyed and loaded from a data file (see
``rb_waveform_core/data/offsets``), not hard-coded. The target Rekordbox version
is auto-detected from the running rekordbox.exe file version, falling back to the
explicitly selected known version. Unknown versions are rejected.

Exposes the same public API as RekordboxLinkListener:
    reader = RekordboxMemoryReader(deck_count=2)
    reader.start()
    event = reader.get_event(timeout=0.0)
    reader.stop()
"""

from __future__ import annotations

import math
import struct
import threading
import time
from pathlib import Path
from queue import Empty, Queue
from typing import Dict, List, Optional, Tuple

from rb_waveform_core.rkbx_link_listener import DeckEvent, DeckState


SAMPLE_RATE = 44100.0
_RECONNECT_INTERVAL = 3.0   # seconds between reconnect attempts
_SLOW_EVERY_N = 25          # ANLZ path check once every N fast ticks

# A pointer is (deref_offsets, final_offset). Resolution matches rkbx_link:
#   addr = module_base
#   for o in deref_offsets: addr = *(addr + o)   # every offset is a dereference
#   result = addr + final_offset
# The first deref offset is module-relative to rekordbox.exe.
Pointer = Tuple[List[int], int]

# Candidate locations for the version-keyed offsets file (first existing wins).
_OFFSETS_FILE_CANDIDATES = [
    Path(__file__).parent / "data" / "offsets",          # packaged with the lib
]


# ---------------------------------------------------------------------------
# Offsets file parsing (format mirrors rkbx_link/src/offsets.rs)
# ---------------------------------------------------------------------------

def _parse_pointer(line: str) -> Pointer:
    """Parse a space-separated hex pointer line into (deref_offsets, final_offset)."""
    parts = [int(tok, 16) for tok in line.split()]
    if not parts:
        raise ValueError("empty pointer line")
    return parts[:-1], parts[-1]


def _read_offset_blocks(text: str) -> List[List[str]]:
    """Split the offsets file into version blocks.

    Blocks are separated by two or more blank lines; '#' lines and blank lines
    are dropped. Each returned block is the list of meaningful lines in order.
    """
    blocks: List[List[str]] = []
    cur: List[str] = []
    empty = 0
    for raw in text.splitlines():
        line = raw.strip()
        if line == "":
            empty += 1
            if empty >= 2 and cur:
                blocks.append(cur)
                cur = []
        else:
            empty = 0
            if not line.startswith("#"):
                cur.append(line)
    if cur:  # flush trailing block (rkbx_link relies on trailing blanks; we don't)
        blocks.append(cur)
    return blocks


def _parse_offsets_file(path: Path) -> Dict[str, dict]:
    """Parse a version-keyed offsets file into {version: offset_table}.

    offset_table = {
        "masterdeck": Pointer,
        "bpm":        [Pointer, ...],   # one per deck
        "sample_pos": [Pointer, ...],
        "anlz_path":  [Pointer, ...],
    }
    Per-deck blocks contain 4 lines each, in order:
    current_bpm, sample_position, track_info, anlz_path (track_info is parsed
    for alignment but unused).
    """
    text = path.read_text(encoding="utf-8")
    versions: Dict[str, dict] = {}
    for block in _read_offset_blocks(text):
        if len(block) < 2:
            continue
        version = block[0]
        masterdeck = _parse_pointer(block[1])
        bpm: List[Pointer] = []
        sample_pos: List[Pointer] = []
        anlz_path: List[Pointer] = []
        rest = block[2:]
        # Consume 4 lines per deck; ignore any trailing partial group.
        for i in range(0, len(rest) - 3, 4):
            bpm.append(_parse_pointer(rest[i]))
            sample_pos.append(_parse_pointer(rest[i + 1]))
            # rest[i + 2] is track_info — parsed only for alignment, not stored
            anlz_path.append(_parse_pointer(rest[i + 3]))
        versions[version] = {
            "masterdeck": masterdeck,
            "bpm": bpm,
            "sample_pos": sample_pos,
            "anlz_path": anlz_path,
        }
    return versions


def _load_offsets() -> Dict[str, dict]:
    """Locate and parse the offsets file from the known candidate paths."""
    for candidate in _OFFSETS_FILE_CANDIDATES:
        if candidate.exists():
            try:
                versions = _parse_offsets_file(candidate)
                print(f"[MemReader] Loaded offsets from {candidate} "
                      f"(versions: {sorted(versions)})")
                return versions
            except Exception as e:
                print(f"[MemReader] Failed to parse offsets file {candidate}: {e}")
    print("[MemReader] No offsets file found in: "
          + ", ".join(str(c) for c in _OFFSETS_FILE_CANDIDATES))
    return {}


# ---------------------------------------------------------------------------
# Rekordbox version detection / selection
# ---------------------------------------------------------------------------

def _version_key(v: str) -> tuple:
    """Sort key: parse dotted-numeric version into a tuple of ints."""
    out = []
    for part in v.split("."):
        try:
            out.append(int(part))
        except ValueError:
            out.append(0)
    return tuple(out)


def _exe_file_version(exe_path: str) -> Optional[str]:
    """Read the Windows file version of an executable as 'major.minor.build.rev'."""
    try:
        import ctypes
        from ctypes import wintypes

        ver = ctypes.windll.version
        ver.GetFileVersionInfoSizeW.restype = wintypes.DWORD
        size = ver.GetFileVersionInfoSizeW(exe_path, None)
        if not size:
            return None
        buf = ctypes.create_string_buffer(size)
        if not ver.GetFileVersionInfoW(exe_path, 0, size, buf):
            return None
        ptr = ctypes.c_void_p()
        length = ctypes.c_uint()
        if not ver.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length)):
            return None

        class VS_FIXEDFILEINFO(ctypes.Structure):
            _fields_ = [
                ("dwSignature", wintypes.DWORD),
                ("dwStrucVersion", wintypes.DWORD),
                ("dwFileVersionMS", wintypes.DWORD),
                ("dwFileVersionLS", wintypes.DWORD),
                ("dwProductVersionMS", wintypes.DWORD),
                ("dwProductVersionLS", wintypes.DWORD),
                ("dwFileFlagsMask", wintypes.DWORD),
                ("dwFileFlags", wintypes.DWORD),
                ("dwFileOS", wintypes.DWORD),
                ("dwFileType", wintypes.DWORD),
                ("dwFileSubtype", wintypes.DWORD),
                ("dwFileDateMS", wintypes.DWORD),
                ("dwFileDateLS", wintypes.DWORD),
            ]

        ffi = ctypes.cast(ptr, ctypes.POINTER(VS_FIXEDFILEINFO)).contents
        ms, ls = ffi.dwFileVersionMS, ffi.dwFileVersionLS
        return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"
    except Exception:
        return None


def _detect_rekordbox_version(pid: int) -> Optional[str]:
    """Detect the Rekordbox version from the running process executable."""
    try:
        import psutil
        exe = psutil.Process(pid).exe()
    except Exception:
        return None
    return _exe_file_version(exe)


def _select_version(detected: Optional[str], available: List[str]) -> Optional[str]:
    """Pick the best matching offsets version for a detected version string.

    Match the release's major/minor/patch, retaining exact build matches first.
    Layout names may include a trailing zero whereas the file has a build number.
    """
    if not available:
        return None
    if detected:
        parts = detected.split(".")
        for n in range(len(parts), 0, -1):
            cand = ".".join(parts[:n])
            if cand in available:
                return cand
        if len(parts) >= 3 and all(part.isdigit() for part in parts):
            release = tuple(int(part) for part in parts[:3])
            for version in available:
                known = version.split('.')
                if len(known) >= 3 and all(part.isdigit() for part in known):
                    if tuple(int(part) for part in known[:3]) == release:
                        return version
    return None


# ---------------------------------------------------------------------------
# Memory access helpers
# ---------------------------------------------------------------------------

def _resolve_chain(pm, base: int, deref_offsets: List[int], final: int) -> int:
    """Walk a pointer chain and return the final address (matches rkbx_link)."""
    addr = base
    for offset in deref_offsets:
        raw = pm.read_bytes(addr + offset, 8)
        addr = struct.unpack_from("<Q", raw)[0]
    return addr + final


def _read_ntstring(pm, addr: int, maxlen: int = 500) -> str:
    """Read a null-terminated byte string from process memory."""
    raw = pm.read_bytes(addr, maxlen)
    end = raw.find(b"\x00")
    return raw[:end].decode("utf-8", errors="replace") if end >= 0 else raw.decode("utf-8", errors="replace")


class RekordboxMemoryReader:
    """Poll Rekordbox process memory and emit DeckEvents, same API as RekordboxLinkListener."""

    def __init__(
        self,
        deck_count: int = 2,
        poll_hz: float = 50.0,
        rekordbox_version: Optional[str] = None,
    ) -> None:
        self.deck_count = max(1, int(deck_count))
        self._poll_interval = 1.0 / max(1.0, poll_hz)
        self._explicit_version = rekordbox_version
        self._all_offsets = _load_offsets()
        self._offsets: Optional[dict] = None  # selected per-version table
        self.version: Optional[str] = None    # selected version string
        self._queue: Queue[DeckEvent] = Queue()
        self._state: Dict[int, DeckState] = {i: DeckState(deck=i) for i in range(self.deck_count)}
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.connected = False
        self.status = "Waiting for Rekordbox"

    # ------------------------------------------------------------------
    # Public API (matches RekordboxLinkListener)
    # ------------------------------------------------------------------

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, daemon=True, name="rb-mem-reader")
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
        self.connected = False

    def get_event(self, timeout: Optional[float] = None) -> Optional[DeckEvent]:
        try:
            return self._queue.get(timeout=timeout)
        except Empty:
            return None

    # ------------------------------------------------------------------
    # Background polling loop
    # ------------------------------------------------------------------

    def _run(self) -> None:
        if not self._all_offsets:
            self.status = "No supported Rekordbox memory layouts are installed"
            return
        while not self._stop_event.is_set():
            pm = self._wait_for_rekordbox()
            if pm is None:
                return
            try:
                base = self._get_module_base(pm)
                if base is None:
                    self.status = "Cannot locate the Rekordbox process module"
                elif self._select_offsets(pm):
                    self.status = f"Rekordbox {self.version}: waiting for readable deck data"
                    self._poll_loop(pm, base)
            except Exception as exc:
                self.status = f"Rekordbox memory connection failed: {exc}"
                print(f"[MemReader] {self.status}")
            finally:
                self.connected = False
                try:
                    pm.close_process()
                except Exception:
                    pass
            if self._stop_event.wait(_RECONNECT_INTERVAL):
                return

    def _select_offsets(self, pm) -> bool:
        """Detect the Rekordbox version and select its offset table. Returns success."""
        available = list(self._all_offsets.keys())
        detected = self._explicit_version
        if detected is None:
            detected = _detect_rekordbox_version(pm.process_id)
            if detected:
                print(f"[MemReader] Detected Rekordbox version: {detected}")
            else:
                print("[MemReader] Could not detect Rekordbox version from process")
        chosen = _select_version(detected, available)
        if chosen is None:
            self.status = f"Unsupported Rekordbox {detected or 'version'}; supported: {', '.join(sorted(available))}"
            print(f"[MemReader] {self.status}")
            return False
        if detected and chosen not in (detected, ".".join(detected.split(".")[:3])):
            print(f"[MemReader] No exact offsets for {detected}; using closest: {chosen}")
        self.version = chosen
        self._offsets = self._all_offsets[chosen]
        n_decks = len(self._offsets["bpm"])
        if self.deck_count > n_decks:
            print(f"[MemReader] Offsets define {n_decks} decks; "
                  f"limiting from {self.deck_count}")
            self.deck_count = n_decks
        return True

    def _wait_for_rekordbox(self):
        """Block until rekordbox.exe is found or stop is requested. Returns a Pymem instance."""
        try:
            import pymem
        except ImportError:
            self.status = "Windows memory component missing: pymem"
            print(f"[MemReader] {self.status}")
            return None

        while not self._stop_event.is_set():
            try:
                pm = pymem.Pymem("rekordbox.exe")
                return pm
            except Exception:
                self.status = "Waiting for Rekordbox (or permission to read its process)"
                print(f"[MemReader] {self.status}")
                self._stop_event.wait(timeout=_RECONNECT_INTERVAL)
        return None

    def _get_module_base(self, pm) -> Optional[int]:
        try:
            import pymem.process
            module = pymem.process.module_from_name(pm.process_handle, "rekordbox.exe")
            return module.lpBaseOfDll if module else None
        except Exception:
            return None

    def _poll_loop(self, pm, base: int) -> None:
        slow_counter = 0
        prev_anlz: Dict[int, str] = {i: "" for i in range(self.deck_count)}
        debugged = False
        last_success = time.monotonic()

        while not self._stop_event.is_set():
            tick_start = time.monotonic()

            try:
                # Fast update: BPM + playhead every tick
                for deck in range(self.deck_count):
                    bpm = self._read_f32(pm, base, *self._offsets["bpm"][deck])
                    sample_pos = self._read_i64(pm, base, *self._offsets["sample_pos"][deck])
                    if bpm is None or not 0 <= bpm <= 1000 or sample_pos is None:
                        continue
                    last_success = time.monotonic()
                    self.connected = True
                    self.status = f"Rekordbox {self.version}: connected"
                    if not debugged:
                        debugged = True
                        path = self._read_anlz_path(pm, base, deck)
                        print(f"[MemReader] First read OK — deck {deck}: bpm={bpm:.2f} "
                              f"t={sample_pos / SAMPLE_RATE:.2f}s anlz={path!r}")
                    seconds = sample_pos / SAMPLE_RATE
                    state = self._state[deck]
                    state.current_bpm = float(bpm)
                    state.time_seconds = float(seconds)
                    state.last_update = time.time()
                    self._queue.put(state.snapshot(source="memory"))

                # Slow update: ANLZ path
                if slow_counter == 0:
                    for deck in range(self.deck_count):
                        path = self._read_anlz_path(pm, base, deck)
                        if path is not None and path != prev_anlz[deck]:
                            prev_anlz[deck] = path
                            state = self._state[deck]
                            state.anlz_path = path
                            state.last_update = time.time()
                            self._queue.put(state.snapshot(source="anlz_path"))

                slow_counter = (slow_counter + 1) % _SLOW_EVERY_N
                if time.monotonic() - last_success > 2.0:
                    self.status = "Rekordbox deck data is unreadable; reconnecting"
                    return

            except Exception as e:
                self.status = f"Rekordbox read error: {e}"
                print(f"[MemReader] {self.status}")
                return  # triggers reconnect

            elapsed = time.monotonic() - tick_start
            sleep_for = max(0.0, self._poll_interval - elapsed)
            if sleep_for > 0:
                self._stop_event.wait(timeout=sleep_for)

    # ------------------------------------------------------------------
    # Typed memory reads
    # ------------------------------------------------------------------

    def _read_f32(self, pm, base: int, deref_offsets: list, final: int) -> Optional[float]:
        try:
            addr = _resolve_chain(pm, base, deref_offsets, final)
            raw = pm.read_bytes(addr, 4)
            value = struct.unpack_from("<f", raw)[0]
            return value if math.isfinite(value) else None
        except Exception:
            return None

    def _read_i64(self, pm, base: int, deref_offsets: list, final: int) -> Optional[int]:
        try:
            addr = _resolve_chain(pm, base, deref_offsets, final)
            raw = pm.read_bytes(addr, 8)
            return struct.unpack_from("<q", raw)[0]
        except Exception:
            return None

    def _read_anlz_path(self, pm, base: int, deck: int) -> Optional[str]:
        try:
            deref_offsets, final = self._offsets["anlz_path"][deck]
            addr = _resolve_chain(pm, base, deref_offsets, final)
            path = _read_ntstring(pm, addr, maxlen=500)
            if not path:
                return ""
            # Rekordbox reports the .DAT file path; the app loads the containing
            # ANLZ folder, so strip the filename (matches the old OSC listener's
            # _normalize_anlz_path behaviour).
            p = Path(path)
            if p.suffix.lower() == ".dat":
                p = p.parent
            return str(p)
        except Exception:
            return None
