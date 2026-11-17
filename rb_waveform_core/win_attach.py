"""Windows: connect Wave's bridge to a Rekordbox that is already running, without restarting it.

Wave loads its bridge library into rekordbox.exe (as Windows loads any library: LoadLibraryW, on a
thread of Rekordbox's) and then runs the bridge's ``wave_bridge_attach`` there. The bridge makes the
same checks as when Rekordbox loads it at start - the exact build, every function it calls - and
answers with an exit code. Rekordbox is never modified on disk, and nothing stays loaded beyond
this Rekordbox session.
"""
from __future__ import annotations

import os
from pathlib import Path
import struct
import sys

ENTRY = 'wave_bridge_attach'
# The bridge's answers (native/win_bridge/wave_bridge_win.cpp, `begin`).
ANSWERS = {
    0: 'connected',
    1: 'Rekordbox already runs a Wave bridge',
    2: 'That process is not Rekordbox',
    3: "The bridge could not open Wave's state file",
    4: 'This Rekordbox build is not supported by the bridge',
    5: "The bridge could not reach Rekordbox's UI thread",
}


class AttachError(OSError):
    pass


def export_rva(image, name=ENTRY):
    """The RVA of the exported function ``name`` in the PE file ``image`` (bytes)."""
    if image[:2] != b'MZ':
        raise AttachError('Not a Windows library')
    pe = struct.unpack_from('<I', image, 0x3c)[0]
    if image[pe:pe + 4] != b'PE\0\0':
        raise AttachError('Not a Windows library')
    sections, optional_size = struct.unpack_from('<H12xH', image, pe + 6)
    optional = pe + 24
    magic = struct.unpack_from('<H', image, optional)[0]
    if magic != 0x20b:
        raise AttachError('The bridge is not a 64-bit library')
    export_dir_rva, _ = struct.unpack_from('<II', image, optional + 112)
    table = []
    for i in range(sections):
        at = optional + optional_size + 40 * i
        size, address, raw_size, raw = struct.unpack_from('<IIII', image, at + 8)
        table.append((address, max(size, raw_size), raw))

    def offset(rva):
        for address, size, raw in table:
            if address <= rva < address + size:
                return raw + rva - address
        raise AttachError('Malformed library')

    if not export_dir_rva:
        raise AttachError('The bridge exports nothing')
    directory = offset(export_dir_rva)
    count_functions, count_names, functions, names, ordinals = struct.unpack_from('<IIIII', image, directory + 20)
    wanted = name.encode()
    for i in range(count_names):
        name_at = offset(struct.unpack_from('<I', image, offset(names) + 4 * i)[0])
        if image[name_at:name_at + len(wanted) + 1] == wanted + b'\0':
            ordinal = struct.unpack_from('<H', image, offset(ordinals) + 2 * i)[0]
            if ordinal >= count_functions:
                break
            return struct.unpack_from('<I', image, offset(functions) + 4 * ordinal)[0]
    raise AttachError(f'This bridge has no {name}: it predates live connecting')


def attach(pid, library, timeout=20.0):
    """Load ``library`` into process ``pid`` and run its attach entry. Returns (connected, message)."""
    if sys.platform != 'win32':
        raise AttachError('Live connecting is for Windows')
    if sys.maxsize <= 2**32:
        raise AttachError('Live connecting needs 64-bit Wave')
    import ctypes
    from ctypes import wintypes
    library = Path(library).resolve()
    rva = export_rva(library.read_bytes())
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    HANDLE, LPVOID, SIZE_T, DWORD, BOOL = wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.BOOL

    def fn(name, restype, *argtypes):
        f = getattr(k32, name)
        f.restype, f.argtypes = restype, list(argtypes)
        return f
    OpenProcess = fn('OpenProcess', HANDLE, DWORD, BOOL, DWORD)
    VirtualAllocEx = fn('VirtualAllocEx', LPVOID, HANDLE, LPVOID, SIZE_T, DWORD, DWORD)
    VirtualFreeEx = fn('VirtualFreeEx', BOOL, HANDLE, LPVOID, SIZE_T, DWORD)
    WriteProcessMemory = fn('WriteProcessMemory', BOOL, HANDLE, LPVOID, ctypes.c_char_p, SIZE_T, ctypes.POINTER(SIZE_T))
    CreateRemoteThread = fn('CreateRemoteThread', HANDLE, HANDLE, LPVOID, SIZE_T, LPVOID, LPVOID, DWORD, ctypes.POINTER(DWORD))
    WaitForSingleObject = fn('WaitForSingleObject', DWORD, HANDLE, DWORD)
    GetExitCodeThread = fn('GetExitCodeThread', BOOL, HANDLE, ctypes.POINTER(DWORD))
    CloseHandle = fn('CloseHandle', BOOL, HANDLE)
    GetModuleHandleW = fn('GetModuleHandleW', LPVOID, wintypes.LPCWSTR)
    GetProcAddress = fn('GetProcAddress', LPVOID, LPVOID, ctypes.c_char_p)

    access = 0x0002 | 0x0400 | 0x0008 | 0x0020 | 0x0010 | 0x00100000  # CREATE_THREAD, QUERY_INFO, VM_OP/WRITE/READ, SYNCHRONIZE
    process = OpenProcess(access, False, int(pid))
    if not process:
        raise AttachError(f'Wave may not open Rekordbox (error {ctypes.get_last_error()}): '
                          'is Rekordbox running as administrator?')
    try:
        def run(start, argument, wait):
            thread = CreateRemoteThread(process, None, 0, start, argument, 0, None)
            if not thread:
                raise AttachError(f'Rekordbox refused a thread (error {ctypes.get_last_error()})')
            try:
                if WaitForSingleObject(thread, int(wait * 1000)) != 0:
                    raise AttachError('Rekordbox did not answer in time')
                code = DWORD()
                GetExitCodeThread(thread, ctypes.byref(code))
                return code.value
            finally:
                CloseHandle(thread)

        path = str(library).encode('utf-16-le') + b'\0\0'
        remote = VirtualAllocEx(process, None, len(path), 0x3000, 0x04)  # COMMIT|RESERVE, READWRITE
        if not remote:
            raise AttachError(f'Rekordbox refused memory (error {ctypes.get_last_error()})')
        try:
            written = SIZE_T()
            if not WriteProcessMemory(process, remote, path, len(path), ctypes.byref(written)) or written.value != len(path):
                raise AttachError('Wave could not hand Rekordbox the bridge')
            # kernel32 sits at the same address in every process of this boot.
            load_library = GetProcAddress(GetModuleHandleW('kernel32.dll'), b'LoadLibraryW')
            if not run(load_library, remote, timeout / 2):
                raise AttachError('Rekordbox could not load the bridge')
        finally:
            VirtualFreeEx(process, remote, 0, 0x8000)  # MEM_RELEASE
        base = module_base(pid, library)
        if not base:
            raise AttachError('The bridge did not stay loaded in Rekordbox')
        code = run(base + rva, None, timeout)
        return code == 0, ANSWERS.get(code, f'The bridge answered {code}')
    finally:
        CloseHandle(process)


def module_base(pid, library):
    """Where ``library`` is loaded in process ``pid``, or 0."""
    import ctypes
    from ctypes import wintypes

    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('th32ModuleID', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                    ('GlblcntUsage', wintypes.DWORD), ('ProccntUsage', wintypes.DWORD), ('modBaseAddr', ctypes.c_void_p),
                    ('modBaseSize', wintypes.DWORD), ('hModule', wintypes.HMODULE), ('szModule', ctypes.c_wchar * 256),
                    ('szExePath', ctypes.c_wchar * 260)]
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    k32.Module32FirstW.argtypes = k32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = k32.CreateToolhelp32Snapshot(0x8 | 0x10, int(pid))  # TH32CS_SNAPMODULE | SNAPMODULE32
    if snapshot in (None, ctypes.c_void_p(-1).value):
        return 0
    wanted = os.path.normcase(str(Path(library).resolve()))
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        more = k32.Module32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if os.path.normcase(entry.szExePath) == wanted:
                return entry.modBaseAddr or 0
            more = k32.Module32NextW(snapshot, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snapshot)
    return 0
