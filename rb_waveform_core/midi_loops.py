"""Loop and cue buttons read from the DJ controller, decoded with rekordbox's own MIDI maps.

Listening is passive: the OS hands controller messages to every client, so
rekordbox receives them unchanged. Nothing is ever sent to the controller.
"""
from __future__ import annotations

import csv
import glob
import os
from pathlib import Path
import platform
import re
import sys
import threading
import time
from queue import Queue, Empty


def mapping_dirs():
    """User MIDI-learn maps first, then those shipped with the newest rekordbox."""
    if sys.platform == 'darwin':
        patterns = [str(Path.home() / 'Library/Pioneer/rekordbox/MidiMappings'),
                    '/Applications/rekordbox*/rekordbox.app/Contents/Resources/MidiMappings']
    else:
        programs = os.environ.get('ProgramFiles', r'C:\Program Files')
        patterns = [os.path.join(os.environ.get('APPDATA', ''), 'Pioneer', 'rekordbox', 'MidiMappings')]
        # Wherever the running rekordbox.exe was installed, then the usual install folders.
        patterns += [os.path.join(os.path.dirname(exe), *parts, 'MidiMappings')
                     for exe in _running_rekordbox() for parts in ((), ('*',), ('*', '*'))]
        patterns += [os.path.join(programs, *parts, 'MidiMappings') for parts in
                     (('Pioneer', 'rekordbox*'), ('Pioneer', 'rekordbox*', '*'), ('rekordbox*',), ('rekordbox', 'rekordbox*'))]
    return [Path(found) for pattern in patterns for found in sorted(glob.glob(pattern), reverse=True)]


def _running_rekordbox():
    try:
        import psutil
        return [p.info['exe'] for p in psutil.process_iter(['name', 'exe'])
                if (p.info['name'] or '').lower() == 'rekordbox.exe' and p.info['exe']]
    except Exception:
        return []


def action(name):
    """rekordbox function name → (kind, value); None for functions that don't change loops or cues."""
    fixed = {'LoopIn': ('in', None), 'LoopOut': ('out', None), 'ReloopExit': ('reloop', None),
             'AutoLoop': ('auto', None), '4BeatLoop': ('auto', None), '4BeatLoopLongPress': ('auto', 4),
             'LoopHalf': ('scale', .5), 'LoopDouble': ('scale', 2), 'Memory': ('memory', None),
             'PlayPause': ('play', None), 'Cue': ('play', None), 'Zoom': ('zoom', 'relative')}
    if name in fixed:
        return fixed[name]
    match = re.fullmatch(r'BeatLoop(1_)?(\d+)', name)
    if match:
        return 'auto', 1 / int(match[2]) if match[1] else int(match[2])
    match = re.fullmatch(r'PAD(\d+)_(BeatLoop|HotCue)(\+Shift)?', name)
    if match:
        kind = {('BeatLoop', None): 'pad', ('HotCue', None): 'hotcue', ('HotCue', '+Shift'): 'delete'}
        return (kind[match[2], match[3]], int(match[1])) if (match[2], match[3]) in kind else None
    return None


def load_map(path):
    """(controller name, {(status, data1): (deck, action)}) from a rekordbox .midi.csv."""
    rows = list(csv.reader(Path(path).read_text(encoding='utf-8-sig', errors='replace').splitlines()))
    name = next((r[2] for r in rows if len(r) > 2 and r[0] == '@file'), Path(path).name.split('.')[0])
    # Some maps name pads by mode number (PAD1_PadMode7); the mode button's comment says
    # which mode that is ("BEAT LOOP Mode On/Off").
    modes = {}
    for row in rows:
        mode = re.fullmatch(r'PadMode(\d+)', row[0]) if row else None
        comment = row[14].upper() if mode and len(row) > 14 else ''
        if 'HOT CUE MODE' in comment or 'BEAT LOOP MODE' in comment:
            modes[mode[1]] = 'HotCue' if 'HOT CUE' in comment else 'BeatLoop'
    names = [re.sub(r'_PadMode(\d+)', lambda m: '_' + modes.get(m[1], m[0][1:]), row[0]) if row else '' for row in rows]
    # Beat-loop pads count from the map's first; rekordbox's default pages run 1/4 .. 32 beats.
    # shortcut: a custom pad length shows as that default until the first wrap corrects it.
    first = min((action(n)[1] for n in names if (action(n) or ('',))[0] == 'pad'), default=1)
    table = {}
    for row, label in zip(rows, names):
        if len(row) < 8 or not re.fullmatch(r'[9Bb][0-9A-Fa-f]{3}', row[3]):
            continue
        kind = action(label)
        if kind is None:
            continue
        if kind[0] == 'pad':
            kind = 'auto', 2 ** ((kind[1] - first) % 8 - 2)
        status, data1 = int(row[3][:2], 16), int(row[3][2:], 16)
        # Deck columns hold the MIDI channel offset each deck sends on.
        offsets = [(deck, int(o)) for deck, o in enumerate(row[4:8]) if o.strip().isdigit() and int(o) < 16]
        for deck, offset in offsets:
            table.setdefault((status + offset, data1), (deck, kind))
        if not offsets:  # a control for all decks, such as waveform zoom
            table.setdefault((status, data1), (None, kind))
    return name, table


def _key(text):
    text = re.sub(r'pioneer( dj)?|midi|\(\d+\)', '', text.casefold())
    return re.sub(r'[^a-z0-9]', '', text)


def find_map(device, maps):
    """The map whose controller name is the longest match inside the device name."""
    device = _key(device)
    best = None
    for name, table in maps:
        key = _key(name)
        if key and key in device and (best is None or len(key) > len(best[0])):
            best = key, name, table
    return best[1:] if best else None


def messages(data):
    """(status, data1, data2) channel messages from a raw MIDI byte stream."""
    status, pending = 0, []
    sysex = False
    for byte in data:
        if byte >= 0xF8:
            continue
        if byte >= 0x80:
            sysex = byte == 0xF0
            status, pending = (byte, []) if byte < 0xF0 else (0, [])
            continue
        if sysex or not status:
            continue
        pending.append(byte)
        if len(pending) == (1 if 0xC0 <= status < 0xE0 else 2):
            yield status, pending[0], pending[-1]
            pending = []  # running status keeps ``status``


class MidiLoopListener:
    def __init__(self):
        self.maps = []
        self.status = ''
        self._load_maps()
        self.devices = {}  # port id → controller map name
        self._queue = Queue()
        self._tables = {}
        self._stop = threading.Event()
        self._thread = None

    def _load_maps(self):
        """rekordbox's maps; retried while missing (on Windows they are found via the running app)."""
        maps, seen = [], set()
        for folder in mapping_dirs():
            for path in sorted(folder.glob('*.midi.csv')):
                try:
                    name, table = load_map(path)
                except (OSError, csv.Error):
                    continue
                if table and name not in seen:  # mixers have no loop buttons
                    seen.add(name)
                    maps.append((name, table))
        self.maps = maps
        if not maps:
            self.status = 'rekordbox MIDI maps not found'
        elif self.status == 'rekordbox MIDI maps not found':
            self.status = ''

    @property
    def controllers(self):
        return sorted(set(self.devices.values()))

    def start(self):
        if self._thread is None:
            self._stop.clear()
            target = self._run_mac if sys.platform == 'darwin' else self._run_windows
            self._thread = threading.Thread(target=target, name='controller-midi', daemon=True)
            self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.5)
            self._thread = None

    def get_event(self):
        try:
            return self._queue.get_nowait()
        except Empty:
            return None

    def _receive(self, port, data):
        stamp = time.monotonic()
        table = self._tables.get(port)
        for status, data1, value in messages(data):
            if not table or (status, data1) not in table:
                continue
            deck, (kind, extra) = table[status, data1]
            if kind == 'zoom':
                self._queue.put((None, 'zoom', (extra, value), stamp))
            elif value:  # presses only: note-on velocity or button CC value above zero
                self._queue.put((deck, kind, extra, stamp))

    def _attach(self, port, name):
        found = find_map(name, self.maps)
        if found:
            self.devices[port] = found[0]
            self._tables[port] = found[1]
        return found is not None

    def _run_mac(self):
        import ctypes as C
        midi = C.CDLL('/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI')
        cf = C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        u32, ptr = C.c_uint32, C.c_void_p
        cf.CFStringCreateWithCString.restype = ptr
        cf.CFStringCreateWithCString.argtypes = [ptr, C.c_char_p, u32]
        cf.CFStringGetCString.argtypes = [ptr, C.c_char_p, C.c_long, u32]
        cf.CFRelease.argtypes = [ptr]
        cf.CFRunLoopRunInMode.argtypes = [ptr, C.c_double, C.c_bool]
        midi.MIDIGetNumberOfSources.restype = C.c_ulong
        midi.MIDIGetSource.restype = u32
        midi.MIDIGetSource.argtypes = [C.c_ulong]
        midi.MIDIObjectGetStringProperty.argtypes = [u32, ptr, C.POINTER(ptr)]
        read_proc = C.CFUNCTYPE(None, ptr, ptr, ptr)
        midi.MIDIClientCreate.argtypes = [ptr, ptr, ptr, C.POINTER(u32)]
        midi.MIDIInputPortCreate.argtypes = [u32, ptr, read_proc, ptr, C.POINTER(u32)]
        midi.MIDIPortConnectSource.argtypes = [u32, u32, ptr]
        midi.MIDIClientDispose.argtypes = [u32]
        display_name = ptr.in_dll(midi, 'kMIDIPropertyDisplayName')
        mode = ptr.in_dll(cf, 'kCFRunLoopDefaultMode')
        aligned = platform.machine() == 'arm64'

        def cfstring(text):
            return cf.CFStringCreateWithCString(None, text.encode(), 0x08000100)

        def packets(packet_list):
            # MIDIPacketList: UInt32 count, then MIDIPacket {UInt64 time; UInt16 length; Byte data[]},
            # packed to 4 bytes; arm64 starts each following packet on a 4-byte boundary.
            at = packet_list + 4
            for _ in range(C.c_uint32.from_address(packet_list).value):
                length = C.c_uint16.from_address(at + 8).value
                yield C.string_at(at + 10, length)
                at += 10 + length
                if aligned:
                    at = (at + 3) & ~3

        @read_proc
        def on_packets(packet_list, _port, source):
            for data in packets(packet_list):
                self._receive(source or 0, data)

        self._callback = on_packets
        client, port = u32(), u32()
        name = cfstring('Rekordbox Wave')
        try:
            if midi.MIDIClientCreate(name, None, None, C.byref(client)) or \
                    midi.MIDIInputPortCreate(client.value, name, on_packets, None, C.byref(port)):
                self.status = 'MIDI unavailable'
                return
            connected = set()
            retry = 0.0
            while not self._stop.is_set():
                if not self.maps and time.monotonic() >= retry:
                    retry = time.monotonic() + 5
                    self._load_maps()
                    connected &= set(self._tables)  # look again at devices seen without a map
                present = set()
                for i in range(midi.MIDIGetNumberOfSources()):
                    source = midi.MIDIGetSource(i)
                    present.add(source)
                    if source in connected:
                        continue
                    text = ptr()
                    label = ''
                    if not midi.MIDIObjectGetStringProperty(source, display_name, C.byref(text)) and text.value:
                        buffer = C.create_string_buffer(256)
                        if cf.CFStringGetCString(text, buffer, 256, 0x08000100):
                            label = buffer.value.decode()
                        cf.CFRelease(text)
                    connected.add(source)
                    if self._attach(source, label):
                        midi.MIDIPortConnectSource(port.value, source, ptr(source))
                for gone in connected - present:
                    self.devices.pop(gone, None)
                    self._tables.pop(gone, None)
                connected &= present
                # The run loop delivers CoreMIDI's device notifications, so new sources appear.
                cf.CFRunLoopRunInMode(mode, 1.0, False)
                self._stop.wait(.5)
        finally:
            if client.value:
                midi.MIDIClientDispose(client.value)
            cf.CFRelease(name)

    def _run_windows(self):
        import ctypes as C
        from ctypes import wintypes as W
        winmm = C.WinDLL('winmm')

        class Caps(C.Structure):
            _fields_ = [('wMid', W.WORD), ('wPid', W.WORD), ('vDriverVersion', W.UINT),
                        ('szPname', W.WCHAR * 32), ('dwSupport', W.DWORD)]

        midi_proc = C.WINFUNCTYPE(None, W.HANDLE, W.UINT, C.c_size_t, C.c_size_t, C.c_size_t)
        winmm.midiInOpen.argtypes = [C.POINTER(W.HANDLE), W.UINT, C.c_size_t, C.c_size_t, W.DWORD]
        winmm.midiInGetDevCapsW.argtypes = [C.c_size_t, C.POINTER(Caps), W.UINT]
        for f in ('midiInStart', 'midiInStop', 'midiInClose'):
            getattr(winmm, f).argtypes = [W.HANDLE]

        @midi_proc
        def on_message(_handle, message, instance, param, _time):
            if message == 0x3C3:  # MIM_DATA: status | data1 << 8 | data2 << 16
                self._receive(instance, bytes((param & 0xFF, param >> 8 & 0x7F, param >> 16 & 0x7F)))

        self._callback = on_message
        handles, names = [], None
        retry = 0.0
        try:
            while not self._stop.is_set():
                if not self.maps and time.monotonic() >= retry:
                    retry = time.monotonic() + 5
                    self._load_maps()
                    if self.maps:
                        names = None  # reopen with maps to match against
                caps = []
                for i in range(winmm.midiInGetNumDevs()):
                    info = Caps()
                    if not winmm.midiInGetDevCapsW(i, C.byref(info), C.sizeof(info)):
                        caps.append((i, info.szPname))
                if [n for _, n in caps] != names:
                    # Port numbers shift when devices come and go: reopen all.
                    for handle in handles:
                        winmm.midiInStop(handle)
                        winmm.midiInClose(handle)
                    handles.clear()
                    self.devices.clear()
                    self._tables.clear()
                    names = [n for _, n in caps]
                    busy = []
                    for i, label in caps:
                        if not find_map(label, self.maps):
                            continue
                        handle = W.HANDLE()
                        if winmm.midiInOpen(C.byref(handle), i, C.cast(on_message, C.c_void_p).value, i + 1, 0x30000):
                            busy.append(label)  # classic Windows MIDI drivers allow one app per port
                            continue
                        self._attach(i + 1, label)
                        handles.append(handle)
                        winmm.midiInStart(handle)
                    self.status = f'{", ".join(busy)} MIDI is held by rekordbox' if busy else ''
                self._stop.wait(2)
        finally:
            for handle in handles:
                winmm.midiInStop(handle)
                winmm.midiInClose(handle)
