"""Rekordbox's own popups - notices, promotions, warnings, questions - never leave Wave stuck.

A watcher looks at Rekordbox's windows about once a second. A popup is either closed for the DJ,
when it is plainly a notice (one acknowledging button, or a "later" kind of button on something
that reads like news or an offer), or brought over Wave so the DJ can answer it. Nothing Wave
can't read is ever pressed.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import logging
import re
import sys
import threading
import time

log = logging.getLogger(__name__)

# One button only, and it just acknowledges: pressing it is all the popup can do.
ACKNOWLEDGE = {'ok', 'close', 'got it', 'done', 'dismiss', 'understood', 'i understand', 'continue', 'x', '×'}
# Buttons that put something off without changing anything.
LATER = {'later', 'not now', 'remind me later', 'maybe later', 'skip', 'no thanks', 'no, thanks', 'not yet', 'ask me later',
         "don't show again", 'do not show again', "don't show this again"}
# Words of a notice or an offer: then closing or cancelling it is also fine.
NOTICE = re.compile(r"\b(what'?s new|new feature|news|update available|new version|version|release notes|tips?|tutorial|"
                    r"subscription|plan|trial|offer|campaign|discount|sale|upgrade|premium|creative|professional|core|"
                    r"cloud|survey|feedback|notice|information|announcement|learn more|introducing)\b", re.I)
SOFT_CLOSE = {'close', 'cancel', 'x', '×'}
# Never dismissed for the DJ: Rekordbox may need these answered.
KEEP = re.compile(r'\b(sign ?in|log ?in|password|account|licen[cs]e|activat|quit|exit|delete|remove|overwrite|'
                  r'database|backup|restore|convert|corrupt|audio device|driver)\b', re.I)


def label(text):
    return re.sub(r'\s+', ' ', str(text or '')).strip().strip('.!').lower()


@dataclass
class Popup:
    key: object                  # the window, as the platform knows it
    title: str = ''
    texts: tuple = ()
    buttons: tuple = ()          # button names, as shown
    first_seen: float = field(default_factory=time.monotonic)

    @property
    def summary(self):
        words = ' · '.join(t for t in (self.title, *self.texts) if t and t.lower() != 'rekordbox')
        return words[:140] or 'Rekordbox is asking something'


def dismiss_with(popup):
    """The button Wave may press to close this popup for the DJ, or None to show it to them."""
    names = [label(b) for b in popup.buttons]
    words = ' '.join((popup.title, *popup.texts))
    if not names or KEEP.search(words):
        return None
    if len(names) == 1 and names[0] in ACKNOWLEDGE:
        return popup.buttons[0]
    for allowed in (LATER, SOFT_CLOSE if NOTICE.search(words) else ()):
        for name, button in zip(names, popup.buttons):
            if name in allowed:
                return button
    return None


class PopupWatcher:
    """Watches Rekordbox's windows on a thread of its own; Wave's UI reads ``popups`` and ``closed``."""

    def __init__(self, backend=None, interval=1.0):
        self.backend = backend
        self.interval = interval
        self.pid = None
        self.auto_close = True
        self.exclude = set()     # windows Wave is showing on purpose (Rekordbox's browser)
        self.popups = []         # popups showing now, oldest first
        self.closed = []         # (time, summary) of popups Wave closed, newest last
        self.error = ''
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        if self._thread is None:
            self._stop.clear()
            self._thread = threading.Thread(target=self._run, name='rekordbox-popups', daemon=True)
            self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
            self._thread = None

    def _run(self):
        backend = self.backend
        try:
            if backend is None:
                backend = WindowsPopups() if sys.platform == 'win32' else MacPopups() if sys.platform == 'darwin' else None
        except Exception as exc:
            self.error = f'Rekordbox popups cannot be read: {exc}'
            log.warning(self.error)
            return
        if backend is None:
            return
        known = {}
        while not self._stop.is_set():
            try:
                found = backend.scan(self.pid, self.exclude) if self.pid else []
                now, current = time.monotonic(), []
                for popup in found:
                    previous = known.get(popup.key)
                    if previous:
                        popup.first_seen = previous.first_seen
                    choice = dismiss_with(popup) if self.auto_close else None
                    # A moment's grace: a popup still drawing has not shown all its buttons yet.
                    if choice is not None and now - popup.first_seen > .6:
                        try:
                            backend.press(popup, choice)
                            log.info('Closed a Rekordbox popup with %r: %s', choice, popup.summary)
                            self.closed = (self.closed + [(now, popup.summary)])[-20:]
                            continue
                        except Exception as exc:
                            log.warning('Could not close Rekordbox popup %r: %s', popup.summary, exc)
                    current.append(popup)
                known = {p.key: p for p in current}
                self.popups = sorted(current, key=lambda p: p.first_seen)
                self.error = ''
            except Exception as exc:
                self.error = str(exc)
                self.popups = []
            finally:
                if hasattr(backend, 'forget'):
                    backend.forget()
            self._stop.wait(self.interval)


class WindowsPopups:
    def __init__(self):
        from . import win_windows
        self.w = win_windows
        self.automation = None
        self.held = []

    def scan(self, pid, exclude):
        _, _, popups = self.w.classify(self.w.windows_of(pid), exclude)
        found = []
        for win in popups:
            texts, buttons = [], []
            try:
                if self.automation is None:
                    self.automation = self.w.Automation()
                texts, buttons = self.automation.read(win.hwnd)
                self.held.extend(buttons)
            except Exception as exc:
                log.debug('UI Automation could not read %s: %s', win.title, exc)
            popup = Popup(win.hwnd, win.title, tuple(t for t in texts if t != win.title), tuple(n for n, _ in buttons))
            popup.elements = dict(buttons)
            found.append(popup)
        return found

    def press(self, popup, button):
        self.automation.press(popup.elements[button])

    def forget(self):
        if self.automation:
            self.automation.release_all(self.held)
        self.held = []


class MacPopups:
    """Rekordbox's windows through the Accessibility API (Wave already asks for it on macOS)."""

    def __init__(self):
        from .mac_ax import Accessibility
        import ctypes as C
        self.ax = Accessibility()
        self.C = C
        bind = lambda lib, name, res, *args: (setattr(getattr(lib, name), 'restype', res),
                                               setattr(getattr(lib, name), 'argtypes', list(args)), getattr(lib, name))[2]
        self.perform = bind(self.ax.ax, 'AXUIElementPerformAction', C.c_int, C.c_void_p, C.c_void_p)
        self.value_get = bind(self.ax.ax, 'AXValueGetValue', C.c_bool, C.c_void_p, C.c_int, C.c_void_p)
        self.true = C.c_void_p.in_dll(self.ax.cf, 'kCFBooleanTrue').value
        self.press_name = self.ax.string(None, b'AXPress', 0x08000100)

    def _size(self, element):
        value = self.ax._attribute(element, 'AXSize')
        if not value:
            return 0.0
        try:
            size = (self.C.c_double * 2)()
            return size[0] * size[1] if self.value_get(value, 2, size) else 0.0  # kAXValueCGSizeType
        finally:
            self.ax.release(value)

    def _children(self, element):
        children = self.ax._attribute(element, 'AXChildren')
        if not children:
            return []
        try:
            if self.ax.type_id(children) != self.ax.array_id():
                return []
            return [self.ax.retain(self.ax.item(children, i)) for i in range(self.ax.count(children))]
        finally:
            self.ax.release(children)

    def scan(self, pid, exclude):
        if not self.ax.trusted():
            raise PermissionError('Allow Rekordbox Wave in Accessibility to handle Rekordbox popups')
        self.ax.clear_elements()
        app = self.ax.application(pid)
        if not app:
            return []
        self.ax.timeout(app, .25)
        windows = self.ax._attribute(app, 'AXWindows')
        self.ax.release(app)
        if not windows:
            return []
        try:
            listed = [self.ax.retain(self.ax.item(windows, i)) for i in range(self.ax.count(windows))]
        finally:
            self.ax.release(windows)
        self.ax.elements.extend(listed)
        sized = [(self._size(w), w) for w in listed]
        largest = max((s for s, _ in sized), default=0)
        found = []
        for area, window in sized:
            main = self.ax._attribute(window, 'AXMain')
            is_main = main == self.true
            if main:
                self.ax.release(main)
            title = self.ax.text(window, 'AXTitle')
            if is_main or area >= largest * .6 or title in exclude or area <= 0:
                continue
            texts, buttons, elements, stack, seen = [], [], {}, [(window, 0)], 0
            while stack and seen < 300:
                element, depth = stack.pop()
                seen += 1
                role = self.ax.text(element, 'AXRole')
                if role == 'AXButton':
                    name = (self.ax.text(element, 'AXTitle') or self.ax.text(element, 'AXDescription')
                            or ('Close' if self.ax.text(element, 'AXSubrole') == 'AXCloseButton' else ''))
                    if name:
                        buttons.append(name)
                        elements[name] = element
                elif role == 'AXStaticText':
                    texts.append(self.ax.text(element) or self.ax.text(element, 'AXTitle'))
                if depth < 10:
                    children = self._children(element)
                    self.ax.elements.extend(children)
                    stack.extend((child, depth + 1) for child in reversed(children))
            # AX hands out a new element each scan: the window's title and size say it is the same one.
            popup = Popup((title, round(area)), title, tuple(t for t in texts if t), tuple(buttons))
            popup.elements = elements
            found.append(popup)
        return found

    def press(self, popup, button):
        if self.perform(popup.elements[button], self.press_name):
            raise OSError('The button did not respond')

    def forget(self):
        self.ax.clear_elements()
