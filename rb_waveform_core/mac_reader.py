"""Read-only Mach transport for Rekordbox on Apple Silicon.

This module never changes Rekordbox's signature, entitlements or library. A
hardened Rekordbox process normally denies task access. The caller must surface
``status``; lack of access is not a successful or simulated connection.

Community address facts (not a cross-platform layout) were checked against
https://github.com/grufkork/rkbx_link/blob/c2f787345f38127120722f686d7463390a58b80d/data/offsets-macos
Only the named versions are accepted. A new build requires new address evidence.
"""
from __future__ import annotations

import ctypes
import math
import platform
import plistlib
import struct
import sys
import time
from pathlib import Path

from .rb_memory_reader import RekordboxMemoryReader, _select_version


class MacMemoryAccessError(OSError):
    pass


class _DyldInfo(ctypes.Structure):
    _fields_ = [("address", ctypes.c_uint64), ("size", ctypes.c_uint64),
                ("format", ctypes.c_uint32)]


class MachProcess:
    """Own one task port; only expose bounded process-memory reads."""

    def __init__(self, pid: int, lib=None):
        if lib is None:
            if sys.platform != "darwin":
                raise MacMemoryAccessError("Mach memory reading requires macOS")
            lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        self.lib = lib
        self.process_id = int(pid)
        self.task = ctypes.c_uint32()
        self.lib.mach_task_self.restype = ctypes.c_uint32
        self.lib.task_for_pid.argtypes = [ctypes.c_uint32, ctypes.c_int,
                                          ctypes.POINTER(ctypes.c_uint32)]
        self.lib.task_for_pid.restype = ctypes.c_int
        self.lib.mach_vm_read_overwrite.argtypes = [ctypes.c_uint32, ctypes.c_uint64,
            ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)]
        self.lib.mach_vm_read_overwrite.restype = ctypes.c_int
        self.lib.task_info.argtypes = [ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        self.lib.task_info.restype = ctypes.c_int
        self.lib.mach_port_deallocate.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        self.lib.mach_port_deallocate.restype = ctypes.c_int
        result = self.lib.task_for_pid(self.lib.mach_task_self(), self.process_id,
                                       ctypes.byref(self.task))
        if result or not self.task.value:
            raise MacMemoryAccessError(
                f"macOS denied read access to Rekordbox (Mach {result}). "
                "The running app lacks an authorized debug entitlement. "
                "Rekordbox Wave has not changed the app or its security settings.")

    def read_bytes(self, address: int, size: int) -> bytes:
        if not self.task.value:
            raise MacMemoryAccessError("The Rekordbox task port is closed")
        if not isinstance(size, int) or not 0 < size <= 1024 * 1024 or address <= 0:
            raise ValueError("Invalid process memory range")
        buffer = ctypes.create_string_buffer(size)
        actual = ctypes.c_uint64()
        result = self.lib.mach_vm_read_overwrite(self.task.value, address, size,
                    ctypes.addressof(buffer), ctypes.byref(actual))
        if result or actual.value != size:
            raise MacMemoryAccessError(
                f"Rekordbox memory read failed (Mach {result}, {actual.value}/{size} bytes)")
        return buffer.raw

    def module_base(self) -> int:
        info = _DyldInfo()
        count = ctypes.c_uint32(ctypes.sizeof(info) // ctypes.sizeof(ctypes.c_uint32))
        result = self.lib.task_info(self.task.value, 17, ctypes.byref(info), ctypes.byref(count))
        if result or not info.address:
            raise MacMemoryAccessError(f"Cannot read Rekordbox image metadata (Mach {result})")
        _, image_count, array = struct.unpack("<IIQ", self.read_bytes(info.address, 16))
        if not 0 < image_count <= 4096 or not array:
            raise MacMemoryAccessError("Invalid Rekordbox image list")
        base, = struct.unpack("<Q", self.read_bytes(array, 8))
        if self.read_bytes(base, 4) != b"\xcf\xfa\xed\xfe":
            raise MacMemoryAccessError("The Rekordbox image is not a 64-bit Mach-O executable")
        return base

    def close_process(self):
        if self.task.value:
            self.lib.mach_port_deallocate(self.lib.mach_task_self(), self.task.value)
            self.task.value = 0


def _community_layout(root: int, analysis_root: int) -> dict:
    # Deck vector offsets and field positions are specific to these arm64 builds.
    return {
        "bpm": [([root, deck * 8, 0x2c8], 0x188) for deck in range(4)],
        "sample_pos": [([root, deck * 8, 0x2c8], 0x120) for deck in range(4)],
        "anlz_path": [([analysis_root, (deck + 1) * 8, 0x3f0], 0) for deck in range(4)],
    }


MAC_LAYOUTS = {
    "7.2.8": _community_layout(0x4d8aa18, 0x4dd0708),
    "7.2.17": _community_layout(0x4ea57a0, 0x4eebc08),
    "7.2.18": _community_layout(0x4eb1588, 0x4ef7b58),
}


def app_version(executable: Path) -> str | None:
    try:
        with (Path(executable).parent.parent / "Info.plist").open("rb") as source:
            info = plistlib.load(source)
        return str(info.get("CFBundleShortVersionString") or "") or None
    except (OSError, plistlib.InvalidFileException):
        return None


class RekordboxMacMemoryReader(RekordboxMemoryReader):
    """DeckEvent reader using Mach, retaining the Windows reader's queue API."""

    def __init__(self, deck_count=4, poll_hz=50.0, rekordbox_version=None):
        super().__init__(deck_count, poll_hz, rekordbox_version)
        self._all_offsets = MAC_LAYOUTS
        self.status = "Waiting for Rekordbox"

    def _find_process(self):
        import psutil
        for process in psutil.process_iter(["pid", "name", "exe"]):
            try:
                if str(process.info.get("name", "")).lower() == "rekordbox":
                    return process.info["pid"], Path(process.info["exe"])
            except (psutil.Error, TypeError):
                continue
        return None

    def _choose_layout(self, detected: str | None, architecture: str) -> bool:
        self.version = None
        self._offsets = None
        if architecture.lower() not in ("arm64", "aarch64"):
            self.status = "No validated Intel Mac Rekordbox memory layout is available"
            return False
        chosen = _select_version(detected, list(MAC_LAYOUTS))
        if chosen is None:
            self.status = (f"No Mac memory layout for Rekordbox {detected or 'unknown version'}; "
                           f"known Apple Silicon layouts: {', '.join(MAC_LAYOUTS)}")
            return False
        self.version = chosen
        self._offsets = MAC_LAYOUTS[chosen]
        return True

    def _poll_loop(self, process, base):
        last_valid = time.monotonic()
        next_path_check = 0.0
        previous_paths = {i: None for i in range(self.deck_count)}
        while not self._stop_event.is_set():
            started = time.monotonic()
            for deck in range(self.deck_count):
                bpm = self._read_f32(process, base, *self._offsets["bpm"][deck])
                samples = self._read_i64(process, base, *self._offsets["sample_pos"][deck])
                if bpm is None or samples is None or not math.isfinite(bpm) or not 0 <= bpm <= 1000:
                    continue
                seconds = samples / 44100.0
                if not -3600 <= seconds <= 86400:
                    continue
                state = self._state[deck]
                state.current_bpm = bpm
                state.time_seconds = seconds
                state.last_update = time.time()
                self._queue.put(state.snapshot(source="memory"))
                last_valid = started
            if started >= next_path_check:
                next_path_check = started + 0.5
                for deck in range(self.deck_count):
                    path = self._read_anlz_path(process, base, deck)
                    if path is not None and path != previous_paths[deck]:
                        previous_paths[deck] = path
                        state = self._state[deck]
                        state.anlz_path = path
                        state.last_update = time.time()
                        self._queue.put(state.snapshot(source="anlz_path"))
            if started - last_valid > 1.0:
                raise MacMemoryAccessError("Lost readable Rekordbox deck data; reconnecting")
            self._stop_event.wait(max(0, self._poll_interval - (time.monotonic() - started)))

    def _run(self):
        if sys.platform != "darwin":
            self.status = "The native Mac reader requires macOS"
            return
        while not self._stop_event.is_set():
            process = None
            try:
                target = self._find_process()
                if target is None:
                    self.status = "Waiting for Rekordbox"
                else:
                    pid, executable = target
                    detected = self._explicit_version or app_version(executable)
                    if self._choose_layout(detected, platform.machine()):
                        process = MachProcess(pid)
                        base = process.module_base()
                        # Validate a readable finite field before calling the connection live.
                        values = [self._read_f32(process, base, *pointer)
                                  for pointer in self._offsets["bpm"]]
                        if not any(v is not None and math.isfinite(v) and 0 <= v <= 1000 for v in values):
                            raise MacMemoryAccessError("The selected Mac memory layout did not produce valid deck data")
                        self.connected = True
                        self.status = f"Connected to Rekordbox {self.version} on Apple Silicon"
                        self._poll_loop(process, base)
            except (MacMemoryAccessError, OSError) as exc:
                self.status = str(exc)
            except Exception as exc:
                self.status = f"Mac connection failed: {exc}"
            finally:
                self.connected = False
                if process is not None:
                    process.close_process()
            self._stop_event.wait(3.0)
