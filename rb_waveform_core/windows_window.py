"""Physical monitor placement after Qt's native full-screen transition."""
import ctypes
import sys
from ctypes import wintypes


def cover_monitor(hwnd):
    if sys.platform != 'win32':
        return
    user32 = ctypes.windll.user32
    class MonitorInfo(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('monitor', wintypes.RECT),
                    ('work', wintypes.RECT), ('flags', wintypes.DWORD)]
    user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
    user32.MonitorFromWindow.restype = wintypes.HANDLE
    user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MonitorInfo)]
    user32.GetMonitorInfoW.restype = wintypes.BOOL
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.SetWindowPos.restype = wintypes.BOOL
    monitor = user32.MonitorFromWindow(hwnd, 2)
    info = MonitorInfo()
    info.size = ctypes.sizeof(info)
    if not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
        raise ctypes.WinError()
    rect = info.monitor
    # rcMonitor covers the panel; rcWork excludes the taskbar and is wrong here.
    if not user32.SetWindowPos(hwnd, -1, rect.left, rect.top,
                              rect.right - rect.left, rect.bottom - rect.top,
                              0x0010 | 0x0020):  # NOACTIVATE | FRAMECHANGED
        raise ctypes.WinError()
