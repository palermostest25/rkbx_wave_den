"""Windows: Rekordbox's top-level windows, placed and raised from Wave, and read through UI Automation.

Only Win32 window calls on Rekordbox's windows and UI Automation (the screen reader interface,
which Rekordbox's JUCE interface provides) are used: nothing of Rekordbox's code is called.
"""
from __future__ import annotations

from dataclasses import dataclass
import sys

GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_THICKFRAME, WS_MAXIMIZEBOX, WS_EX_TOOLWINDOW = 0x00040000, 0x00010000, 0x00000080
HWND_TOPMOST, HWND_NOTOPMOST, HWND_BOTTOM = -1, -2, 1
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_SHOWWINDOW = 0x1, 0x2, 0x10, 0x40
SW_MINIMIZE, SW_RESTORE, SW_SHOWNOACTIVATE, SW_SHOWMINNOACTIVE = 6, 9, 4, 7


@dataclass(frozen=True)
class Win:
    hwnd: int
    title: str
    visible: bool
    minimized: bool
    rect: tuple          # left, top, right, bottom (the restored rectangle when minimized)
    owned: bool
    resizable: bool
    tool: bool

    @property
    def area(self):
        left, top, right, bottom = self.rect
        return max(0, right - left) * max(0, bottom - top)


def classify(windows, presenting=()):
    """(main, panels, popups) among Rekordbox's windows. The main window is titled 'rekordbox' (the
    largest, if several); panels are its other resizable windows, such as the Split Screen browser;
    popups are what is left and showing: questions, warnings, notices."""
    shown = [w for w in windows if (w.visible or w.minimized) and not w.tool]
    named = [w for w in shown if w.title.strip().lower() == 'rekordbox' and not w.owned]
    main = max(named or [w for w in shown if not w.owned and w.resizable] or [None],
               key=lambda w: w.area if w else 0)
    panels, popups = [], []
    for w in shown:
        if w is main:
            continue
        if w.hwnd in presenting or (w.resizable and main and w.area > main.area * .25):
            panels.append(w)
        elif w.visible and not w.minimized and w.area > 0:
            popups.append(w)
    return main, panels, popups


if sys.platform == 'win32':
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    HWND = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.IsWindowVisible.argtypes = user32.IsIconic.argtypes = user32.IsWindow.argtypes = [HWND]
    user32.GetWindow.argtypes = [HWND, wintypes.UINT]
    user32.GetWindow.restype = HWND
    user32.GetWindowLongW.argtypes = [HWND, ctypes.c_int]
    user32.GetWindowRect.argtypes = [HWND, ctypes.POINTER(wintypes.RECT)]
    user32.SetWindowPos.argtypes = [HWND, HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.ShowWindow.argtypes = [HWND, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = [HWND]
    user32.BringWindowToTop.argtypes = [HWND]
    user32.GetForegroundWindow.restype = HWND

    class WINDOWPLACEMENT(ctypes.Structure):
        _fields_ = [('length', wintypes.UINT), ('flags', wintypes.UINT), ('showCmd', wintypes.UINT),
                    ('ptMinPosition', wintypes.POINT), ('ptMaxPosition', wintypes.POINT),
                    ('rcNormalPosition', wintypes.RECT)]
    user32.GetWindowPlacement.argtypes = [HWND, ctypes.POINTER(WINDOWPLACEMENT)]


def windows_of(pid):
    """Every top-level window of process ``pid``."""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            found.append(describe(hwnd))
        return True
    user32.EnumWindows(each, 0)
    return [w for w in found if w]


def describe(hwnd):
    if not user32.IsWindow(hwnd):
        return None
    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title, 256)
    style = user32.GetWindowLongW(hwnd, GWL_STYLE) & 0xffffffff
    exstyle = user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xffffffff
    minimized = bool(user32.IsIconic(hwnd))
    if minimized:
        placement = WINDOWPLACEMENT()
        placement.length = ctypes.sizeof(placement)
        user32.GetWindowPlacement(hwnd, ctypes.byref(placement))
        r = placement.rcNormalPosition
    else:
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
    return Win(int(hwnd), title.value, bool(user32.IsWindowVisible(hwnd)), minimized, (r.left, r.top, r.right, r.bottom),
               bool(user32.GetWindow(hwnd, 4)), bool(style & (WS_THICKFRAME | WS_MAXIMIZEBOX)), bool(exstyle & WS_EX_TOOLWINDOW))


def alive(hwnd):
    return bool(hwnd) and bool(user32.IsWindow(hwnd)) and bool(user32.IsWindowVisible(hwnd))


def present(hwnd, rect=None, centre=None, activate=True):
    """Put a Rekordbox window above Wave's always-on-top window: into ``rect`` (left, top, right,
    bottom, physical pixels), or centred at ``centre``, or where it is."""
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE if activate else SW_SHOWNOACTIVATE)
    flags = SWP_SHOWWINDOW | (0 if activate else SWP_NOACTIVATE)
    if rect:
        left, top, right, bottom = rect
        user32.SetWindowPos(hwnd, HWND_TOPMOST, left, top, right - left, bottom - top, flags)
    elif centre:
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        width, height = r.right - r.left, r.bottom - r.top
        user32.SetWindowPos(hwnd, HWND_TOPMOST, int(centre[0] - width / 2), int(centre[1] - height / 2), 0, 0,
                            flags | SWP_NOSIZE)
    else:
        user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE)
    if activate:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)


def put_back(hwnd, minimize=True):
    """Undo ``present``: no longer above everything, and out of sight if asked."""
    if not user32.IsWindow(hwnd):
        return
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    if minimize:
        user32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)


def window_rect(hwnd):
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def split_screen_from_menu(hwnd):
    """View › Show Split Screen through the window's menu, when Rekordbox has a native one. True if sent."""
    user32.GetMenu.argtypes = [HWND]
    user32.GetMenu.restype = wintypes.HMENU
    menu = user32.GetMenu(hwnd)
    if not menu:
        return False
    user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
    user32.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]
    user32.GetSubMenu.argtypes = [wintypes.HMENU, ctypes.c_int]
    user32.GetSubMenu.restype = wintypes.HMENU
    user32.GetMenuItemID.argtypes = [wintypes.HMENU, ctypes.c_int]
    user32.PostMessageW.argtypes = [HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    text = ctypes.create_unicode_buffer(128)

    def label(m, i):
        user32.GetMenuStringW(m, i, text, 128, 0x400)  # MF_BYPOSITION
        return text.value.replace('&', '').strip().lower()
    for i in range(user32.GetMenuItemCount(menu)):
        if label(menu, i) != 'view':
            continue
        sub = user32.GetSubMenu(menu, i)
        for j in range(user32.GetMenuItemCount(sub) if sub else 0):
            if label(sub, j).split('\t')[0] in ('show split screen', 'split screen'):
                user32.PostMessageW(hwnd, 0x0111, user32.GetMenuItemID(sub, j) & 0xffff, 0)  # WM_COMMAND
                return True
    return False


# UI Automation, through its COM interface with plain ctypes (method slots per UIAutomationClient.h).
class Automation:
    """Reads a window's text and buttons, and presses a button, as a screen reader would."""
    CLSID = '{ff48dba4-60ef-4201-aa87-54103eef594e}'
    IID = '{30cbe57d-d9d0-452a-ab13-7ac5ac4825ee}'
    INVOKE_IID = '{fb377fbe-8ea6-46d5-9c73-6499642d3059}'
    BUTTON, TEXT = 50000, 50020

    def __init__(self):
        ole32 = ctypes.OleDLL('ole32')
        ole32.CoInitializeEx(None, 2)  # this thread: apartment threaded
        self.oleaut = ctypes.WinDLL('oleaut32')
        self.oleaut.SysFreeString.argtypes = [ctypes.c_void_p]
        self.automation = ctypes.c_void_p()
        ole32.CoCreateInstance(ctypes.byref(self.guid(self.CLSID)), None, 1, ctypes.byref(self.guid(self.IID)),
                               ctypes.byref(self.automation))
        self.true = self.call(self.automation, 21, ctypes.c_void_p)

    @staticmethod
    def guid(text):
        class GUID(ctypes.Structure):
            _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]
        hexed = text.strip('{}').replace('-', '')
        return GUID(int(hexed[:8], 16), int(hexed[8:12], 16), int(hexed[12:16], 16),
                    (ctypes.c_ubyte * 8)(*bytes.fromhex(hexed[16:])))

    @staticmethod
    def method(obj, slot, *argtypes):
        vtable = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        return ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, *argtypes)(vtable[slot])

    def call(self, obj, slot, out_type, *args, argtypes=()):
        out = out_type()
        hr = self.method(obj, slot, *argtypes, ctypes.POINTER(out_type))(obj, *args, ctypes.byref(out))
        if hr < 0:
            raise OSError(f'UI Automation call failed ({hr & 0xffffffff:#x})')
        return out.value

    def release(self, obj):
        if obj:
            self.method(obj, 2)(obj)

    def name(self, element):
        bstr = self.call(element, 23, ctypes.c_void_p)
        try:
            return ctypes.wstring_at(bstr) if bstr else ''
        finally:
            self.oleaut.SysFreeString(bstr)

    def read(self, hwnd):
        """(texts, buttons) of the window: texts as strings, buttons as (name, element) pairs.
        Release the elements with ``release_all``."""
        window = self.call(self.automation, 6, ctypes.c_void_p, ctypes.c_void_p(hwnd), argtypes=(ctypes.c_void_p,))
        texts, buttons = [self.name(window)], []
        found = self.call(window, 6, ctypes.c_void_p, 4, ctypes.c_void_p(self.true),
                          argtypes=(ctypes.c_int, ctypes.c_void_p))  # FindAll(TreeScope_Descendants, true)
        self.release(window)
        try:
            for i in range(min(self.call(found, 3, ctypes.c_int), 200)):
                element = self.call(found, 4, ctypes.c_void_p, i, argtypes=(ctypes.c_int,))
                kind = self.call(element, 21, ctypes.c_int)
                if kind == self.BUTTON:
                    buttons.append((self.name(element), element))
                    continue
                if kind == self.TEXT:
                    texts.append(self.name(element))
                self.release(element)
        finally:
            self.release(found)
        return [t for t in texts if t], buttons

    def press(self, element):
        pattern = self.call(element, 14, ctypes.c_void_p, 10000, ctypes.byref(self.guid(self.INVOKE_IID)),
                            argtypes=(ctypes.c_int, ctypes.c_void_p))  # GetCurrentPatternAs(Invoke)
        try:
            hr = self.method(pattern, 3)(pattern)
            if hr < 0:
                raise OSError('The button did not respond')
        finally:
            self.release(pattern)

    def release_all(self, buttons):
        for _, element in buttons:
            self.release(element)
