"""One native window: waveform display and integrated, readable settings."""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path

from PySide6.QtCore import Qt, QTimer, Signal, QObject, QStandardPaths, QEvent, QUrl
from PySide6.QtGui import QColor, QFont, QKeySequence, QShortcut, QDesktopServices
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QFormLayout, QLabel, QPushButton, QComboBox, QCheckBox,
    QDoubleSpinBox, QSpinBox, QSlider, QScrollArea, QStackedWidget,
    QFileDialog, QColorDialog, QFrame, QMessageBox, QTabWidget)

from .config import WaveformColorConfig, WaveformRenderConfig, config_to_dict, dict_to_config
from .cues import CueMarker, _marker_color, live_cues
from .deck_controller import DeckController
from .displays import deck_order
from .live_state import PlaybackTimeline
from .qt_display import WaveformCanvas, screen_key, find_screen, bind_fullscreen


def config_directory():
    override = os.environ.get('RKBX_WAVE_CONFIG_DIR')
    if override:
        return Path(override)
    base = Path.home() / 'Library/Application Support' if sys.platform == 'darwin' else Path(os.environ.get('APPDATA', Path.home()))
    return base / 'rkbx_wave'


# Three-band palettes. Rekordbox: flat-fill pixels measured from rekordbox 7's decks; the
# blue and amber match a published rekordbox-style Mixxx skin (#0055E1, #FFA600).
STYLES = {
    'Rekordbox': dict(low_color=(0, 84, 226), mid_color=(255, 166, 0), overlap_color=(180, 102, 7), high_color=(246, 236, 217)),
    'Wave classic': dict(low_color=(20, 95, 235), mid_color=(244, 164, 20), overlap_color=(169, 107, 39), high_color=(255, 255, 255)),
}

# 2.2.0's first Rekordbox palette, sampled with edge blending; migrated to the measured one.
FIRST_REKORDBOX = dict(low_color=(0, 80, 224), mid_color=(248, 160, 0), overlap_color=(176, 96, 0), high_color=(244, 234, 214))


def snap_to_beat(seconds, grid, tolerance=.08):
    """Quantized loops sit on beats; an inferred edge within tolerance is the beat itself."""
    nearest = min((beat.time_ms / 1000 for beat in grid or ()), key=lambda t: abs(t - seconds), default=None)
    return nearest if nearest is not None and abs(nearest - seconds) <= tolerance else seconds


class DisplayLinkRepaint(QObject):
    """Cocoa delivers window update requests on the display link: requesting the next
    one from each keeps exactly one repaint per refresh, which a timer only approximates."""
    def __init__(self, canvas):
        super().__init__(canvas)
        self.canvas = canvas
        self.window = None

    def attach(self):
        window = self.canvas.window().windowHandle()
        if window is None:
            return
        if window is not self.window:
            if self.window is not None:
                self.window.removeEventFilter(self)
            self.window = window
            window.installEventFilter(self)
        window.requestUpdate()

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.UpdateRequest and self.canvas.isVisible():
            # The window repaints itself for this request; ask for the next refresh.
            obj.requestUpdate()
        return False


class Session(QObject):
    changed = Signal()
    zoomRequested = Signal(float)  # multiply the visible beats
    analysisChanged = Signal()
    statusChanged = Signal(str)

    def __init__(self, *, demo=False, config_path=None):
        super().__init__()
        self.path = Path(config_path) if config_path else config_directory() / 'default_config.json'
        self.colors = WaveformColorConfig(**STYLES['Rekordbox'])
        self.render = WaveformRenderConfig()
        self.view = dict(zoom_beats=32, playhead=.32, track_headers=False,
                         grid=True, cues=True, loops=True, bar_numbers=True,
                         source='Native', settings_scale=1.0, always_on_top=True,
                         refresh_hz=60, style='Rekordbox')
        self.decks = {i: DeckController() for i in range(4)}
        self.timelines = {i: PlaybackTimeline() for i in range(4)}
        self.errors = {}
        self.reader = None
        self.reader_error = ''
        self.midi = None
        self.auto_beats = {i: 4 for i in range(4)}
        # Cue edits shown at the press, kept until master.db agrees: (time, cue, added).
        self.cue_edits = {i: [] for i in range(4)}
        self.cue_poll = None
        self.next_cues = 0.0
        self.demo = demo
        self.jobs = {}
        self.requested = {}
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='waveform-analysis')
        self.vsync_wait = None
        self.status = ''
        self.load_config(self.path, initial=True)
        if demo:
            from .demo import populate_demo
            populate_demo(self.decks)
            self.started = time.monotonic()
            self.set_status('DEMO · synthetic analysis')
        else:
            self.connect_source()
            from .midi_loops import MidiLoopListener
            self.midi = MidiLoopListener()
            self.midi.start()
        self.timer = QTimer(self)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.timeout.connect(self.tick)
        self.timer.start(round(1000 / self.view['refresh_hz']))
        self.next_status = 0.0

    def order(self):
        return deck_order(self.render.deck_order, self.render.deck_count)

    def set_status(self, message):
        if message != self.status:
            self.status = message
            self.statusChanged.emit(message)

    def load_config(self, path, initial=False):
        try:
            data = json.loads(Path(path).read_text())
            if not isinstance(data, dict) or any(not isinstance(data.get(k, {}), dict) for k in ("color", "render", "view")):
                raise ValueError("Profile sections must be JSON objects")
            colors, render = dict_to_config(data)
            view = dict(self.view)
            view.update(data.get('view', {}))
            if view.get('source') not in ('Native', 'OSC', 'Offline'):
                raise ValueError('Invalid playback source')
            for key, lo, hi in (('zoom_beats', 4, 512), ('playhead', .15, .75), ('settings_scale', 1, 2), ('refresh_hz', 20, 120)):
                value = float(view[key])
                if not math.isfinite(value) or not lo <= value <= hi:
                    raise ValueError(f'Invalid {key}')
            if "view" not in data and tuple(colors.low_color) == (0, 120, 255):
                colors.low_color = (20, 95, 235)
                if tuple(colors.mid_color) == (212, 155, 70):
                    colors.mid_color = (244, 164, 20)
            saved = {k: tuple(getattr(colors, k)) for k in STYLES['Rekordbox']}
            untouched_classic = 'style' not in data.get('view', {}) and all(
                saved[k] == v for k, v in STYLES['Wave classic'].items() if k != 'overlap_color')
            if untouched_classic or saved == FIRST_REKORDBOX:
                for key, value in STYLES['Rekordbox'].items():  # untouched older palettes: move to the current look
                    setattr(colors, key, value)
            self.colors, self.render, self.view = colors, render, view
            # Previous monitor bindings used SDL names and may not identify a Qt screen.
            # Do not silently select a different physical monitor on migration.
        except FileNotFoundError:
            if not initial:
                raise
        except (OSError, ValueError, TypeError, KeyError) as exc:
            if not initial:
                raise
            self.errors[-1] = f'Previous settings could not be loaded: {exc}'

    def save(self, path=None):
        target = Path(path) if path else self.path
        target.parent.mkdir(parents=True, exist_ok=True)
        data = config_to_dict(self.colors, self.render)
        data['view'] = self.view
        temporary = target.with_suffix('.tmp')
        temporary.write_text(json.dumps(data, indent=2))
        temporary.replace(target)

    def connect_source(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        self.reader_error = ''
        source = self.view['source']
        if source == 'Offline':
            self.set_status('OFFLINE · open an analysis file in Settings')
            return
        try:
            if source == 'OSC':
                from .rkbx_link_listener import RekordboxLinkListener
                self.reader = RekordboxLinkListener(decks=range(4))
            elif sys.platform == 'win32':
                from .rb_memory_reader import RekordboxMemoryReader
                self.reader = RekordboxMemoryReader(deck_count=4)
            elif sys.platform == 'darwin':
                from .mac_accessibility_reader import RekordboxAccessibilityReader
                self.reader = RekordboxAccessibilityReader(deck_count=4)
                # The UI clock ticks in tenths: hold through slow pitch, treat it as a quantum,
                # and ease its larger timing errors in slowly enough to stay invisible.
                for timeline in self.timelines.values():
                    timeline.extrapolate_seconds = .3
                    timeline.stale_seconds = .6
                    timeline.resolution = .1
                    timeline.smoothing_seconds = .6
            else:
                raise RuntimeError('Native Rekordbox connection requires Windows or macOS')
            self.reader.start()
            self.set_status('Waiting for Rekordbox')
        except Exception as exc:
            self.reader = None
            self.reader_error = str(exc)
            self.set_status(self.reader_error)

    def open_track(self, idx, folder):
        path = Path(folder)
        if path.is_file():
            path = path.parent
        old = self.jobs.pop(idx, None)
        if old:
            old.cancel()
        self.requested[idx] = str(path)
        self.cue_edits[idx] = []
        self.decks[idx] = DeckController()
        self.errors[idx] = 'Loading track analysis…'
        self.analysisChanged.emit()
        def load():
            deck = DeckController()
            deck.load_anlz(path)
            if deck.analysis is None or not len(deck.analysis.low):
                raise ValueError('No detailed waveform in this folder')
            return deck
        self.jobs[idx] = self.worker.submit(load)
        self.changed.emit()

    def receive(self, event):
        idx = event.deck
        if idx not in self.decks:
            return
        if getattr(event, 'error', None):
            self.errors[idx] = event.error
        if event.source == 'anlz_path' and not event.anlz_path:
            job = self.jobs.pop(idx, None)
            if job:
                job.cancel()
            self.requested.pop(idx, None)
            self.decks[idx] = DeckController()
            self.timelines[idx].reset()
            self.errors.pop(idx, None)
            return
        if event.anlz_path and str(event.anlz_path) != self.requested.get(idx):
            self.open_track(idx, event.anlz_path)
            self.timelines[idx].reset()
        deck = self.decks[idx]
        deck.update_live_bpm(event.current_bpm)
        timeline = self.timelines[idx]
        timeline.rate_hint = deck.time_scale if deck.original_bpm and deck.current_bpm else None
        timeline.beat_seconds = 60 / deck.original_bpm if deck.original_bpm else None
        if event.time_seconds is not None and event.source in ('time', 'memory', 'accessibility'):
            stamp = time.monotonic() + min(0, event.received_at - time.time())
            inferred = timeline.inferred_loop
            timeline.observe(event.time_seconds, observed_at=stamp,
                playing=getattr(event, 'playing', None), rate=getattr(event, 'rate', None),
                loop_start=getattr(event, 'loop_start', None), loop_end=getattr(event, 'loop_end', None),
                loop_active=getattr(event, 'loop_active', None))
            if timeline.inferred_loop and timeline.inferred_loop != inferred:
                timeline.inferred_loop = tuple(snap_to_beat(t, deck.beat_grid_full) for t in timeline.inferred_loop)
        if event.source in ('loop', 'playing', 'rate'):
            timeline = self.timelines[idx]
            for field in ('loop_start', 'loop_end', 'loop_active', 'playing', 'rate'):
                value = getattr(event, field, None)
                if value is not None:
                    setattr(timeline, field, value)

    def loop_action(self, idx, kind, beats, stamp):
        """Mirror a controller loop or cue press the moment it happens, as rekordbox will play it."""
        if kind == 'zoom':
            # rekordbox's own waveform zoom (SHIFT + BROWSE on most controllers), an endless
            # encoder: 1 = one step clockwise, 127 = one step back.
            value = beats[1]
            steps = value if value < 64 else value - 128
            if steps:
                self.zoomRequested.emit(2 ** (-steps / 4))
            return
        timeline, deck = self.timelines[idx], self.decks[idx]
        if kind == 'play':
            timeline.play_pressed = stamp
            return
        position = timeline.position(stamp)
        if position is None or not deck.original_bpm:
            return
        beat = 60 / deck.original_bpm
        def snap(t):
            # rekordbox's default quantize puts loop and cue points on the nearest beat.
            return snap_to_beat(t, deck.beat_grid_full, beat / 2)
        loop = timeline.active_loop()
        if kind in ('hotcue', 'delete', 'memory'):
            hot = kind != 'memory'
            same = [c for c in deck.cues if ((not c.is_memory and c.number == beats) if hot else
                    (c.is_memory and abs(c.time_sec - snap(position)) < .05))]
            if kind == 'delete':
                edits = [(time.monotonic(), c, False) for c in same]
            elif same:
                return  # a set pad only jumps; a memory cue is already there
            else:
                edits = [(time.monotonic(), CueMarker(snap(position), _marker_color(not hot, beats or 0), not hot, beats or 0), True)]
            # Undo the edits already on screen to get back master.db's view, then reapply all.
            shown = self.cue_edits[idx]
            base = [c for c in deck.cues if not any(c is cue for _, cue, added in shown if added)]
            self.cue_edits[idx] = shown + edits
            self.apply_cues(idx, base + [cue for _, cue, added in shown if not added])
        elif kind == 'in' and not loop:
            timeline.pending_in = snap(position)
        elif kind == 'out' and not loop and timeline.pending_in is not None:
            if snap(position) > timeline.pending_in:
                timeline.engage(timeline.pending_in, snap(position))
        elif kind == 'reloop':
            if loop:
                timeline.exit_loop()
            elif timeline.last_loop:
                timeline.engage(*timeline.last_loop)
        elif kind == 'auto':
            if loop and beats is None:
                timeline.exit_loop()
            else:
                beats = beats or self.auto_beats[idx]
                start = loop[0] if loop else snap(position)
                timeline.engage(start, start + beats * beat)
        elif kind == 'scale':
            self.auto_beats[idx] = min(512, max(1 / 32, self.auto_beats[idx] * beats))
            if loop:
                timeline.engage(loop[0], loop[0] + (loop[1] - loop[0]) * beats)

    def apply_cues(self, idx, cues):
        """Show master.db's cues plus controller edits it hasn't recorded yet."""
        now = time.monotonic()
        cues = list(cues)
        pending = []
        for stamp, cue, added in self.cue_edits[idx]:
            def same(c):
                return c.is_memory == cue.is_memory and (
                    abs(c.time_sec - cue.time_sec) < .1 if cue.is_memory else c.number == cue.number)
            if any(map(same, cues)) == added or now - stamp > 3:
                continue  # recorded (or rekordbox didn't make it)
            pending.append((stamp, cue, added))
            cues = cues + [cue] if added else [c for c in cues if not same(c)]
        self.cue_edits[idx] = pending
        self.decks[idx].cues = sorted(cues, key=lambda c: (c.time_sec, c.number))

    def position(self, idx):
        if self.demo:
            return 40 + (time.monotonic() - self.started) % 100
        timeline = self.timelines[idx]
        position = timeline.sample_position if timeline.stale() else timeline.position()
        if timeline.resolution and timeline.rate == 0 and position is not None:
            # A paused coarse clock only says "within this tenth"; a cue there is where it stopped.
            base = timeline.sample_position
            cue = next((c.time_sec for c in self.decks[idx].cues
                        if base - 1e-3 <= c.time_sec < base + timeline.resolution), None)
            if cue is not None:
                return cue
        return position

    def tick(self):
        if self.reader:
            # Limit work per frame if a sender floods the event queue.
            for _ in range(512):
                event = self.reader.get_event(timeout=0)
                if event is None:
                    break
                self.receive(event)
        while self.midi:
            event = self.midi.get_event()
            if event is None:
                break
            self.loop_action(*event)
        for timeline in self.timelines.values():
            timeline.controller = bool(self.midi and self.midi.controllers)
        self.poll_cues(now := time.monotonic())
        for idx, future in list(self.jobs.items()):
            if not future.done():
                continue
            del self.jobs[idx]
            try:
                deck = future.result()
                if self.decks[idx].current_bpm:
                    deck.update_live_bpm(self.decks[idx].current_bpm)
                self.decks[idx] = deck
                self.analysisChanged.emit()
                self.errors.pop(idx, None)
                if self.view['source'] == 'Offline':
                    self.timelines[idx].observe(0)
            except Exception as exc:
                self.errors[idx] = f'Analysis unavailable: {exc}'
        if self.reader and now >= self.next_status:
            self.next_status = now + .5
            if self.reader.connected:
                controllers = ' · '.join(self.midi.controllers or [self.midi.status]) if self.midi else ''
                self.set_status('REKORDBOX · connected' + (f' · {controllers}' if controllers else ''))
            else:
                self.set_status(getattr(self.reader, 'status', 'Waiting for Rekordbox'))
        if self.vsync_wait:
            self.vsync_wait()
        self.changed.emit()

    def poll_cues(self, now):
        """Re-read loaded tracks' cues a few times a second, so edits made anywhere appear."""
        if self.cue_poll and self.cue_poll.done():
            for idx, (folder, cues) in self.cue_poll.result().items():
                if cues is not None and folder == self.requested.get(idx) and idx not in self.jobs:
                    self.apply_cues(idx, cues)
            self.cue_poll = None
        if self.cue_poll is None and self.requested and not self.demo and now >= self.next_cues:
            self.next_cues = now + .25
            folders = dict(self.requested)
            # The analysis worker owns the database connection, so reads stay on its thread.
            self.cue_poll = self.worker.submit(lambda: {i: (f, live_cues(f)) for i, f in folders.items()})

    def close(self):
        self.timer.stop()
        if self.reader:
            self.reader.stop()
        if self.midi:
            self.midi.stop()
        self.worker.shutdown(wait=False, cancel_futures=True)


class MainWindow(QMainWindow):
    def __init__(self, session):
        super().__init__()
        self.session = session
        self.setWindowTitle('Rekordbox Wave')
        self.setMinimumSize(640, 400)
        self.resize(1280, 800)
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, bool(self.session.view["always_on_top"]))
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.root = QWidget()
        self.setCentralWidget(self.root)
        self.layout = QVBoxLayout(self.root)
        self.layout.setContentsMargins(0, 0, 0, 0)
        self.layout.setSpacing(0)
        self.toolbar = QFrame()
        self.toolbar.setObjectName('toolbar')
        top = QHBoxLayout(self.toolbar)
        top.setContentsMargins(20, 8, 16, 8)
        title = QLabel('WAVEFORM')
        title.setObjectName('title')
        top.addWidget(title)
        top.addSpacing(22)
        self.two = self.button('2 DECK', lambda: self.set_count(2))
        self.four = self.button('4 DECK', lambda: self.set_count(4))
        top.addWidget(self.two)
        top.addWidget(self.four)
        top.addStretch()
        self.status = QLabel(session.status)
        self.status.setObjectName('connection')
        self.status.setMaximumWidth(510)
        top.addWidget(self.status)
        self.settings_button = self.button('SETTINGS', self.toggle_settings)
        self.settings_button.setAccessibleName('Open display settings')
        top.addWidget(self.settings_button)
        self.layout.addWidget(self.toolbar)
        self.pages = QStackedWidget()
        self.canvas = WaveformCanvas(session)
        self.canvas.settingsRequested.connect(self.toggle_settings)
        self.canvas.zoomBy.connect(self.zoom_by)
        session.zoomRequested.connect(self.zoom_by)
        # A pinch or knob turn zooms many times a second; save the result once it settles.
        self.save_zoom = QTimer(self)
        self.save_zoom.setSingleShot(True)
        self.save_zoom.setInterval(600)
        self.save_zoom.timeout.connect(self.persist)
        self.pages.addWidget(self.canvas)
        self.settings_page = self.build_settings()
        self.pages.addWidget(self.settings_page)
        self.layout.addWidget(self.pages, 1)
        if sys.platform == 'darwin':
            self.display_link = DisplayLinkRepaint(self.canvas)
        else:
            self.display_link = None
            self.session.changed.connect(self.canvas.update)
        self.session.statusChanged.connect(self.update_status)
        self.screen_check = QTimer(self)
        self.screen_check.timeout.connect(self.ensure_display)
        # Also follows refresh-rate changes and restarts display-link painting after hide/minimize.
        self.screen_check.timeout.connect(self.pace_frames)
        self.screen_check.start(1500)
        self.connected_screen = None
        # Esc/F11 out of full screen holds a window until the monitor is plugged in again.
        self.hold_windowed = False
        self._bound_at = -1e9
        self._settings_dirty = False
        self._zoom_keys_down = [False, False]
        self.session.changed.connect(self.poll_global_zoom)
        self.apply_theme()
        self.sync_count()
        for key, action in [('F2', self.toggle_settings), ('F11', self.toggle_fullscreen), ('Escape', self.escape),
                            ('+', lambda: self.zoom(-1)), ('=', lambda: self.zoom(-1)), ('-', lambda: self.zoom(1))]:
            shortcut = QShortcut(QKeySequence(key), self)
            shortcut.activated.connect(action)
        app = QApplication.instance()
        app.screenAdded.connect(lambda _: self.screens_changed(plugged=True))
        app.screenRemoved.connect(lambda _: self.screens_changed(plugged=False))
        QTimer.singleShot(0, self.ensure_display)
        QTimer.singleShot(0, self.pace_frames)

    def pace_frames(self):
        screen = self.screen() or QApplication.primaryScreen()
        hz = max(30.0, screen.refreshRate() if screen else 60.0)
        if self.display_link:
            self.display_link.attach()
        elif sys.platform == 'win32':
            import ctypes
            # Wake just before the refresh and let the compositor release the paint, one per refresh.
            self.session.vsync_wait = ctypes.windll.dwmapi.DwmFlush
            interval = max(1, math.floor(1000 / hz) - 4)
        else:
            interval = max(1, round(1000 / hz))
        if not self.display_link and self.session.timer.interval() != interval:
            self.session.timer.setInterval(interval)

    def button(self, title, action):
        button = QPushButton(title)
        button.clicked.connect(action)
        return button

    def update_status(self, text):
        self.status.setText(text if len(text) < 70 else text[:67] + '…')
        self.status.setToolTip(text)
        if hasattr(self, 'connection_detail'):
            self.connection_detail.setText(text)

    def apply_theme(self):
        size = round(22 * self.session.view.get('settings_scale', 1))
        check_icon = (Path(__file__).parent / "data/check.svg").as_posix()
        self.setStyleSheet('''
            QMainWindow, QWidget { background:#000; color:#eeeeee; font-family:Arial; }
            QFrame#toolbar { border-bottom:1px solid #303030; }
            QLabel#title { font-size:24px; font-weight:600; }
            QLabel#connection { color:#a9b0b9; font-size:14px; }
            QPushButton { border:1px solid #383c42; background:#101113; padding:9px 16px; font-size:16px; }
            QPushButton:hover { background:#202731; border-color:#00a6ff; }
            QPushButton:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border:2px solid #00a6ff; }
            QPushButton[selected="true"] { color:#00a6ff; border-color:#008eda; }
            QScrollArea { border:0; }
            QWidget#settings { background:#111418; }
            QWidget#settings QLabel { background:transparent; }
            QComboBox, QDoubleSpinBox, QSpinBox { background:#20252c; padding:9px 12px; border:1px solid #4d5662; min-height:30px; }
            QComboBox QAbstractItemView { background:#20252c; selection-background-color:#1765a3; }
            QCheckBox { spacing:14px; padding:8px 0; background:transparent; }
            QTabWidget::pane { border:1px solid #353c45; background:#111418; }
            QTabBar::tab { background:#161a20; color:#adb8c6; padding:14px 26px; min-width:135px; }
            QTabBar::tab:selected { color:#00b4ff; background:#242b34; }
            QTabBar::tab:focus { border:1px solid #00b4ff; }
            QCheckBox::indicator { width:25px; height:25px; }
            QCheckBox::indicator:unchecked { background:#20252c; border:1px solid #778390; }
            QCheckBox::indicator:checked { background:#009fe9; border:1px solid #77d6ff; image:none; }
            QLabel#section { color:#fff; font-size:25px; font-weight:600; padding-top:14px; }
            QLabel#help { color:#abb6c3; font-size:16px; }
            QScrollBar:vertical { background:#111418; width:15px; }
            QScrollBar::handle:vertical { background:#4c5968; min-height:40px; }
        ''' + f'QCheckBox::indicator:checked {{ image:url("{check_icon}"); }}\nQWidget#settings, QWidget#settings * {{ font-size:{size}px; }}\nQWidget#settings QPushButton {{ min-height:30px; }}')

    def build_settings(self):
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        content.setObjectName('settings')
        outer = QVBoxLayout(content)
        outer.setContentsMargins(34, 24, 34, 34)
        outer.setSpacing(18)
        first = QHBoxLayout()
        first.addWidget(QLabel('Display settings'))
        first.addStretch()
        first.addWidget(self.button('BACK TO WAVEFORMS', self.toggle_settings))
        outer.addLayout(first)
        tabs = QTabWidget()
        outer.addWidget(tabs)
        def tab(title):
            page = QWidget()
            layout = QVBoxLayout(page)
            layout.setContentsMargins(26, 22, 26, 26)
            form = QFormLayout()
            form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
            form.setHorizontalSpacing(32)
            form.setVerticalSpacing(20)
            layout.addLayout(form)
            layout.addStretch()
            area = QScrollArea()
            area.setWidgetResizable(True)
            area.setWidget(page)
            tabs.addTab(area, title)
            return form
        display_form = tab('DISPLAY')
        waveform_form = tab('WAVEFORM')
        connection_form = tab('CONNECTION')
        profiles_form = tab('PROFILES')
        form = display_form
        self.screen_combo = QComboBox()
        self.screen_combo.setAccessibleName('Target monitor')
        self.screen_combo.activated.connect(self.select_screen)
        form.addRow('Monitor', self.screen_combo)
        self.refresh_screens()
        self.fullscreen_check = QCheckBox('Cover the entire monitor, including the taskbar area')
        self.fullscreen_check.setChecked(self.session.render.fill_display)
        self.fullscreen_check.toggled.connect(self.change_fullscreen)
        form.addRow('Full screen', self.fullscreen_check)
        self.order_combo = QComboBox()
        for label, order in [('FLX10 · 3, 1, 2, 4', '3,1,2,4'), ('Numeric · 1, 2, 3, 4', '1,2,3,4'), ('Paired · 1, 3, 2, 4', '1,3,2,4'), ('Reverse · 4, 3, 2, 1', '4,3,2,1')]:
            self.order_combo.addItem(label, order)
        self.order_combo.setCurrentIndex(max(0, self.order_combo.findData(self.session.render.deck_order)))
        self.order_combo.setEditable(True)
        self.order_combo.currentIndexChanged.connect(lambda _: self.set_custom_order())
        self.order_combo.lineEdit().editingFinished.connect(self.set_custom_order)
        form.addRow('Deck order', self.order_combo)
        form = waveform_form
        self.zoom_combo = QComboBox()
        for beats in (4, 8, 16, 32, 64, 128, 256, 512):
            self.zoom_combo.addItem(f'{beats // 4} bars · {beats} beats', beats)
        beats = self.session.view['zoom_beats']
        self.zoom_combo.setCurrentIndex(min(range(self.zoom_combo.count()), key=lambda i: abs(math.log2(self.zoom_combo.itemData(i) / beats))))
        self.zoom_combo.currentIndexChanged.connect(lambda _: self.set_view('zoom_beats', self.zoom_combo.currentData()))
        form.addRow('Waveform zoom', self.zoom_combo)
        self.position_combo = QComboBox()
        for label, fraction in [('AZ position · 32%', .32), ('Centre · 50%', .5), ('Left · 20%', .2)]:
            self.position_combo.addItem(label, fraction)
        self.position_combo.setCurrentIndex(max(0, self.position_combo.findData(self.session.view['playhead'])))
        self.position_combo.currentIndexChanged.connect(lambda _: self.set_view('playhead', self.position_combo.currentData()))
        form.addRow('Playhead', self.position_combo)
        for label, key in [('Track titles and BPM', 'track_headers'), ('Beat and bar markers', 'grid'),
                           ('Bar numbers', 'bar_numbers'), ('Hot cues and memory cues', 'cues'), ('Saved and live loops', 'loops')]:
            box = QCheckBox(label)
            box.setChecked(bool(self.session.view.get(key, True)))
            box.toggled.connect(lambda value, k=key: self.set_view(k, value))
            form.addRow('', box)
        form = display_form
        scale = QDoubleSpinBox()
        scale.setRange(1.0, 2.0)
        scale.setSingleStep(.1)
        scale.setSuffix(' ×')
        scale.setValue(self.session.view['settings_scale'])
        scale.valueChanged.connect(self.set_settings_scale)
        form.addRow('Settings text size', scale)
        wave_scale = QDoubleSpinBox()
        wave_scale.setRange(.75, 2)
        wave_scale.setSingleStep(.1)
        wave_scale.setSuffix(' ×')
        wave_scale.setValue(self.session.render.ui_scale)
        wave_scale.valueChanged.connect(lambda x: self.set_render('ui_scale', x))
        form.addRow('Cue and deck text size', wave_scale)
        form = waveform_form
        band_row = QWidget()
        band_layout = QHBoxLayout(band_row)
        band_layout.setContentsMargins(0, 0, 0, 0)
        for band in ('low', 'mid', 'high'):
            band_layout.addWidget(self.button(band.upper(), lambda _, b=band: self.pick_color(b)))
        form.addRow('Three-band colours', band_row)
        style = QComboBox()
        style.setAccessibleName('Waveform style')
        style.addItems(list(STYLES))
        style.setCurrentText(self.session.view.get('style', 'Rekordbox'))
        style.textActivated.connect(self.set_style)
        form.addRow('Waveform style', style)
        for label, field in [('Low level', 'low_gain'), ('Mid level', 'mid_gain'), ('High level', 'high_gain')]:
            spin = QDoubleSpinBox()
            spin.setRange(.1, 3)
            spin.setSingleStep(.1)
            spin.setSuffix(' ×')
            spin.setValue(getattr(self.session.render, field))
            spin.valueChanged.connect(lambda v, f=field: self.set_render(f, v, invalidate=True))
            form.addRow(label, spin)
        form = connection_form
        self.source_combo = QComboBox()
        self.source_combo.addItems(['Native', 'OSC', 'Offline'])
        self.source_combo.setCurrentText(self.session.view['source'])
        self.source_combo.currentTextChanged.connect(self.change_source)
        form.addRow('Playback connection', self.source_combo)
        self.connection_detail = QLabel(self.session.status)
        self.connection_detail.setWordWrap(True)
        self.connection_detail.setObjectName('help')
        form.addRow('Connection status', self.connection_detail)
        if sys.platform == 'darwin':
            form.addRow(self.button('OPEN ACCESSIBILITY SETTINGS', lambda:
                QDesktopServices.openUrl(QUrl('x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility'))))
        open_row = QWidget()
        open_layout = QHBoxLayout(open_row)
        open_layout.setContentsMargins(0, 0, 0, 0)
        self.open_deck = QComboBox()
        for idx in range(4):
            self.open_deck.addItem(f'Deck {idx + 1}', idx)
        open_layout.addWidget(self.open_deck)
        open_layout.addWidget(self.button('OPEN ANALYSIS FILE…', self.open_analysis))
        form.addRow('Track analysis', open_row)
        profiles = QHBoxLayout()
        profiles.addWidget(self.button('IMPORT PROFILE…', self.import_profile))
        profiles.addWidget(self.button('EXPORT PROFILE…', self.export_profile))
        profiles.addStretch()
        profiles_form.addRow(profiles)
        help_label = QLabel('F2: settings    F11: full screen    + / −: zoom    Escape: back / leave full screen')
        help_label.setObjectName('help')
        help_label.setWordWrap(True)
        outer.addWidget(help_label)
        scroll.setWidget(content)
        return scroll

    def refresh_screens(self):
        if not hasattr(self, 'screen_combo'):
            return
        self.screen_combo.blockSignals(True)
        self.screen_combo.clear()
        self.screen_combo.addItem('Windowed · move freely', '')
        for screen in QApplication.screens():
            rect = screen.geometry()
            ratio = screen.devicePixelRatio()
            self.screen_combo.addItem(f'{screen.name()} · {round(rect.width()*ratio)} × {round(rect.height()*ratio)}', screen_key(screen))
        index = self.screen_combo.findData(self.session.render.display_key)
        self.screen_combo.setCurrentIndex(max(0, index))
        self.screen_combo.blockSignals(False)

    def select_screen(self, index):
        self.session.render.display_key = self.screen_combo.itemData(index)
        # Selecting a physical monitor means filling it, not positioning a small window.
        self.session.render.fill_display = bool(self.session.render.display_key)
        self.fullscreen_check.blockSignals(True)
        self.fullscreen_check.setChecked(self.session.render.fill_display)
        self.fullscreen_check.blockSignals(False)
        self.connected_screen = None
        self.hold_windowed = False
        if self.session.render.display_key:
            self.ensure_display()
        elif self.isFullScreen():
            self.leave_fullscreen()
        self.persist()

    def screens_changed(self, plugged):
        self.refresh_screens()
        if plugged:
            self.hold_windowed = False
        self.ensure_display()

    def ensure_display(self):
        """Fill the remembered monitor whenever it is connected; otherwise stay a window."""
        key = self.session.render.display_key
        if not key or not self.session.render.fill_display:
            return
        screen = find_screen(key, QApplication.screens())
        if screen is None:
            if self.connected_screen is not None:
                self.leave_fullscreen()
                self.update_status('Monitor unplugged · windowed until it is connected again')
            return
        if self.hold_windowed:
            return
        # macOS animates into full screen for about a second and may not report it meanwhile;
        # re-binding then would leave full screen and start again. Only Windows needs the
        # size check, where the taskbar can take part of the monitor back.
        if screen is self.connected_screen and time.monotonic() - self._bound_at < 3:
            return
        resized = sys.platform == 'win32' and self.geometry() != screen.geometry()
        if screen is not self.connected_screen or not self.isFullScreen() or resized:
            self.bind(screen)

    def bind(self, screen):
        bind_fullscreen(self, screen)
        self.connected_screen = screen
        self._bound_at = time.monotonic()

    def leave_fullscreen(self):
        self.connected_screen = None
        self.showNormal()
        area = QApplication.primaryScreen().availableGeometry() if self.screen() is None else self.screen().availableGeometry()
        width, height = min(1280, area.width()), min(800, area.height())
        self.setGeometry(area.x() + (area.width() - width) // 2, area.y() + (area.height() - height) // 2, width, height)

    def change_fullscreen(self, enabled):
        self.session.render.fill_display = enabled
        self.hold_windowed = False
        if enabled:
            # The monitor the window is on now becomes the one to return to.
            screen = self.screen() or QApplication.primaryScreen()
            self.session.render.display_key = screen_key(screen)
            self.refresh_screens()
            self.bind(screen)
        else:
            self.leave_fullscreen()
        self.persist()

    def toggle_fullscreen(self):
        if self.isFullScreen():
            self.hold_windowed = True
            self.leave_fullscreen()
        else:
            self.fullscreen_check.blockSignals(True)
            self.fullscreen_check.setChecked(True)
            self.fullscreen_check.blockSignals(False)
            self.change_fullscreen(True)

    def toggle_settings(self):
        opening = self.pages.currentIndex() == 0
        self.pages.setCurrentIndex(1 if opening else 0)
        self.settings_button.setText('BACK' if opening else 'SETTINGS')
        if opening:
            self.refresh_screens()
        else:
            self.canvas.setFocus()
            self.pace_frames()

    def escape(self):
        if self.pages.currentIndex():
            self.toggle_settings()
        elif self.isFullScreen():
            self.toggle_fullscreen()

    def sync_count(self):
        for button, count in ((self.two, 2), (self.four, 4)):
            button.setProperty('selected', count == self.session.render.deck_count)
            button.style().unpolish(button)
            button.style().polish(button)

    def set_count(self, count):
        self.session.render.deck_count = count
        self.sync_count()
        self.canvas.update()
        self.persist()

    def set_custom_order(self):
        text = self.order_combo.currentText()
        raw = self.order_combo.currentData() if self.order_combo.currentIndex() >= 0 else text
        try:
            deck_order(raw, self.session.render.deck_count)
        except ValueError:
            self.update_status('Deck order must use each number 1, 2, 3, 4 once')
            return
        self.set_render('deck_order', raw)

    def poll_global_zoom(self):
        if sys.platform != 'win32' or self.isActiveWindow():
            return
        import ctypes
        for idx, (key, direction) in enumerate(((self.session.render.zoom_in_key, -1), (self.session.render.zoom_out_key, 1))):
            down = bool(ctypes.windll.user32.GetAsyncKeyState(key) & 0x8000)
            if down and not self._zoom_keys_down[idx]:
                self.zoom(direction)
            self._zoom_keys_down[idx] = down

    def zoom(self, direction):
        self.zoom_by(2 if direction > 0 else .5)

    def zoom_by(self, factor):
        """Smooth zoom from a trackpad, wheel, controller or key: 4 to 512 beats across the lane."""
        beats = max(4.0, min(512.0, self.session.view['zoom_beats'] * factor))
        self.session.view['zoom_beats'] = beats
        nearest = min(range(self.zoom_combo.count()), key=lambda i: abs(math.log2(self.zoom_combo.itemData(i) / beats)))
        self.zoom_combo.blockSignals(True)
        self.zoom_combo.setCurrentIndex(nearest)
        self.zoom_combo.blockSignals(False)
        self.canvas.update()
        self.save_zoom.start()

    def set_view(self, key, value):
        self.session.view[key] = value
        self.canvas.update()
        self.persist()

    def set_render(self, key, value, invalidate=False):
        setattr(self.session.render, key, value)
        if invalidate:
            self.canvas.tiles.clear()
        self.canvas.update()
        self.persist()

    def set_settings_scale(self, value):
        self.session.view['settings_scale'] = value
        self.apply_theme()
        self.persist()

    def set_style(self, name):
        for key, value in STYLES[name].items():
            setattr(self.session.colors, key, value)
        self.session.view['style'] = name
        self.canvas.tiles.clear()
        self.canvas.update()
        self.persist()

    def pick_color(self, band):
        color = QColorDialog.getColor(QColor(*getattr(self.session.colors, band + '_color')), self, f'{band.capitalize()} waveform band')
        if color.isValid():
            setattr(self.session.colors, band + '_color', (color.red(), color.green(), color.blue()))
            self.canvas.tiles.clear()
            self.canvas.update()
            self.persist()

    def change_source(self, source):
        self.session.demo = False
        self.session.view['source'] = source
        self.session.connect_source()
        self.persist()

    def open_analysis(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Open track analysis', '', 'Rekordbox analysis (*.DAT *.EXT *.2EX);;All files (*)')
        if path:
            if self.session.demo:
                self.source_combo.setCurrentText('Offline')
            self.session.open_track(self.open_deck.currentData(), path)

    def import_profile(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Import profile', '', 'JSON profiles (*.json)')
        if not path:
            return
        try:
            self.session.load_config(path)
            self.session.connect_source()
            self.canvas.tiles.clear()
            old = self.settings_page
            self.settings_page = self.build_settings()
            self.pages.removeWidget(old)
            old.deleteLater()
            self.pages.addWidget(self.settings_page)
            self.pages.setCurrentWidget(self.settings_page)
            self.connected_screen = None
            self.ensure_display()
            self.sync_count()
            self.apply_theme()
            self.persist()
        except Exception as exc:
            QMessageBox.warning(self, 'Profile could not be loaded', str(exc))

    def export_profile(self):
        path, _ = QFileDialog.getSaveFileName(self, 'Export profile', 'rekordbox-wave.json', 'JSON profiles (*.json)')
        if path:
            try:
                self.session.save(path)
            except OSError as exc:
                QMessageBox.warning(self, 'Profile could not be saved', str(exc))

    def persist(self):
        try:
            self.session.save()
        except OSError as exc:
            self.update_status(f'Settings could not be saved: {exc}')

    def closeEvent(self, event):
        self.screen_check.stop()
        self.persist()
        self.session.close()
        super().closeEvent(event)


def main():
    parser = argparse.ArgumentParser(description='Rekordbox Wave')
    parser.add_argument('--demo', action='store_true', help='Use labelled synthetic waveform analysis')
    parser.add_argument('--config', type=Path, help='Use a specific settings file')
    parser.add_argument('--smoke-test', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName('Rekordbox Wave')
    app.setOrganizationName('Rekordbox Wave')
    app.setStyle('Fusion')
    session = Session(demo=args.demo, config_path=args.config)
    window = MainWindow(session)
    window.show()
    if args.smoke_test:
        def capture_and_close():
            try:
                args.smoke_test.parent.mkdir(parents=True, exist_ok=True)
                if not window.isVisible() or not window.grab().save(str(args.smoke_test)):
                    raise RuntimeError('Packaged window did not render')
            except Exception:
                app.exit(1)
                return
            window.close()
        QTimer.singleShot(750, capture_and_close)
    return app.exec()
