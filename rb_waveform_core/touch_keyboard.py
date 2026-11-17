"""Windows' own touch keyboard: off while Wave runs (Wave has its own), back as it was when Wave closes.

Windows 11 keeps "Show the touch keyboard" (Settings › Time & language › Typing › Touch keyboard) in
HKCU\\Software\\Microsoft\\TabletTip\\1.7\\TouchKeyboardTapInvoke: 0 never, 1 when no keyboard is
attached, 2 always. Windows 10's switch "Show the touch keyboard when not in tablet mode and there's no
keyboard attached" is EnableDesktopModeAutoInvoke in the same key. Only the signed-in user's setting
changes; nothing needs administrator rights.
"""
from __future__ import annotations

import logging
import sys

log = logging.getLogger(__name__)

KEY = r'Software\Microsoft\TabletTip\1.7'
NEVER, NO_KEYBOARD, ALWAYS = 0, 1, 2
NAMES = {NEVER: 'Never', NO_KEYBOARD: 'When no keyboard is attached', ALWAYS: 'Always'}


def available():
    return sys.platform == 'win32'


def mode(registry=None):
    """Windows' setting now: NEVER, NO_KEYBOARD or ALWAYS (Windows' default, NO_KEYBOARD, when unset)."""
    registry = registry or _registry()
    value = registry.get('TouchKeyboardTapInvoke')
    if value in (NEVER, NO_KEYBOARD, ALWAYS):
        return value
    legacy = registry.get('EnableDesktopModeAutoInvoke')
    return NEVER if legacy == 0 else NO_KEYBOARD


def set_mode(value, registry=None):
    registry = registry or _registry()
    registry.set('TouchKeyboardTapInvoke', int(value))
    registry.set('EnableDesktopModeAutoInvoke', 0 if value == NEVER else 1)
    registry.announce()
    log.info('Windows touch keyboard: %s', NAMES.get(value, value))


class Guard:
    """Turns the touch keyboard off while Wave runs and puts Windows' setting back when it closes. The
    setting to put back is kept in Wave's settings, so a Wave that crashed restores it the next time."""

    def __init__(self, view, save, registry=None):
        self.view, self.save, self.registry = view, save, registry

    def start(self):
        if not available() and self.registry is None or not self.view.get('touch_keyboard_off', True):
            return
        try:
            if self.view.get('touch_keyboard_restore') is None:
                self.view['touch_keyboard_restore'] = mode(self.registry)
                self.save()
            set_mode(NEVER, self.registry)
        except OSError as exc:
            log.warning('Could not turn the touch keyboard off: %s', exc)

    def stop(self):
        original = self.view.get('touch_keyboard_restore')
        if original is None:
            return
        try:
            set_mode(original, self.registry)
            self.view['touch_keyboard_restore'] = None
            self.save()
        except OSError as exc:
            log.warning('Could not put the touch keyboard back: %s', exc)

    def choose(self, value):
        """The DJ picked a setting by hand: it applies now, and it is what Wave leaves behind on closing."""
        try:
            set_mode(value, self.registry)
        except OSError as exc:
            log.warning('Could not set the touch keyboard: %s', exc)
            return False
        if self.view.get('touch_keyboard_restore') is not None:
            self.view['touch_keyboard_restore'] = value
            self.save()
        return True


class _WindowsRegistry:
    def get(self, name):
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as key:
                return winreg.QueryValueEx(key, name)[0]
        except OSError:
            return None

    def set(self, name, value):
        import winreg
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(value))

    def announce(self):
        # As Settings does: tell running programs (the touch keyboard's host among them) that it changed.
        try:
            import ctypes
            result = ctypes.c_size_t()
            ctypes.windll.user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0, 'TabletTip', 0x0002, 1000, ctypes.byref(result))
        except Exception:
            pass


def _registry():
    if not available():
        raise OSError('The touch keyboard setting is a Windows one')
    return _WindowsRegistry()
