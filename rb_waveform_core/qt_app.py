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
    QFileDialog, QColorDialog, QFrame, QMessageBox, QListWidget)

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
# How each source's clock behaves. The Mac UI clock ticks in tenths: hold through slow pitch,
# treat it as a quantum, and ease its larger timing errors in slowly enough to stay invisible.
DEFAULT_TIMING = dict(extrapolate_seconds=.08, stale_seconds=.25, resolution=0.0, smoothing_seconds=.3)
TIMING = {'accessibility': dict(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)}

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


def track_loaded(before, after):
    """True when a deck Wave already knew got a new track; a deck first seen, or emptied, is not a load."""
    return any(path and idx in before and before[idx] != path for idx, path in after.items())


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
    loadStatus = Signal(str)

    def __init__(self, *, demo=False, config_path=None):
        super().__init__()
        self.path = Path(config_path) if config_path else config_directory() / 'default_config.json'
        self.colors = WaveformColorConfig(**STYLES['Rekordbox'])
        self.render = WaveformRenderConfig()
        self.view = dict(zoom_beats=32, playhead=.32, deck_info=True, time_mode='remain', info_size=1.0,
                         artwork=True, end_warning=30, start_rekordbox=True, hide_rekordbox=True, check_updates=True,
                         grid=True, cues=True, loops=True, bar_numbers=True,
                         source='Native', settings_scale=1.0, always_on_top=True,
                         refresh_hz=60, style='Rekordbox')
        self.decks = {i: DeckController() for i in range(4)}
        self.timelines = {i: PlaybackTimeline(infer_loops=demo) for i in range(4)}
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
        # deck -> title, artist, key and artwork as the library has the loaded track
        self.meta = {}
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='waveform-analysis')
        # Load requests wait for Rekordbox's answer off the UI thread; one at a time.
        self.loader = ThreadPoolExecutor(max_workers=1, thread_name_prefix='deck-loader')
        self.loading = None  # (deck, track, request, deadline) until Rekordbox's engine has the file
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
            if sys.platform == 'darwin':
                # Only for SHIFT+BROWSE zoom. CoreMIDI shares a controller between apps; Windows MIDI
                # gives a port to one app, and taking the FLX10 from Rekordbox would stop its decks.
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

    def load_unavailable(self):
        """Why touch loading can't be used now, or '' when it can."""
        if not hasattr(self.reader, 'load'):
            return "Touch loading needs Wave's Rekordbox bridge: Rekordbox 7.2.19 started by Wave (Native connection)"
        if not self.reader.bridged:
            return self.reader.RELAUNCH
        return ''

    def deck_playing(self, idx):
        return bool(self.timelines[idx].playing)

    def load_track(self, idx, track, source=''):
        """Ask Rekordbox to load a track from the collection, or from the USB export at ``source``;
        loadStatus reports the engine's answer."""
        why = self.load_unavailable()
        if not why and self.loading:
            why = f'Still loading {self.loading[1].title}'
        if not why and self.deck_playing(idx):
            why = f'Deck {idx + 1} is playing: stop it before loading over it'
        if not why and not str(track.identifier).isdigit():
            why = 'Only Rekordbox tracks can be loaded from here'
        if why:
            self.loadStatus.emit(why)
            return False
        # A stick's export stores paths from its own root; the engine reports the mounted path.
        root = Path(source).parents[2] if source else None
        expected = str(root / track.path.lstrip('/')) if root else track.path
        request = self.loader.submit(self.reader.load, idx, int(track.identifier), str(root) if root else None)
        self.loading = (idx, replace(track, path=expected), request, time.monotonic() + 8)
        self.loadStatus.emit(f'Loading {track.title} on deck {idx + 1}…')
        return True

    def toggle_master_tempo(self, idx):
        """Key lock on or off for a deck, by Rekordbox's own engine; the deck state shows the result."""
        status = getattr(self.reader, 'deck_status', {}).get(idx)
        if status is None or not hasattr(self.reader, 'set_master_tempo'):
            self.set_status(self.load_unavailable() or 'Master tempo needs the Rekordbox bridge')
            return
        self.loader.submit(self.reader.set_master_tempo, idx, not status.master_tempo)

    def poll_loading(self, now):
        if not self.loading:
            return
        idx, track, request, deadline = self.loading
        if request.done():
            try:
                accepted, message = request.result()
            except Exception as exc:
                accepted, message = False, str(exc)
            if not accepted:
                self.loading = None
                self.loadStatus.emit(f'Deck {idx + 1}: {message}')
                return
            # Done only when Rekordbox's engine reports this very file on this deck.
            loaded = getattr(self.reader, 'deck_paths', {}).get(idx)
            if loaded and track.path and Path(loaded).as_posix().casefold() == Path(track.path).as_posix().casefold():
                self.loading = None
                self.loadStatus.emit(f'Loaded {track.title} on deck {idx + 1}')
                return
        if now > deadline:
            self.loading = None
            self.loadStatus.emit(f'Rekordbox did not load {track.title} on deck {idx + 1}')

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
            if view.get('time_mode') not in ('remain', 'elapsed'):
                raise ValueError('Invalid time display')
            for key, lo, hi in (('zoom_beats', 4, 512), ('playhead', .15, .75), ('settings_scale', 1, 2), ('refresh_hz', 20, 120),
                                ('info_size', .5, 2), ('end_warning', 0, 600)):
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
        for timeline in self.timelines.values():
            timeline.infer_loops = False
            timeline.reset()
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
            elif sys.platform in ('win32', 'darwin'):
                from .mac_bridge import RekordboxBridgeReader, launch, rekordbox_pid
                self.reader = RekordboxBridgeReader(deck_count=4)
                # Rekordbox is the engine behind Wave: start it, out of sight, when it isn't running.
                if self.view.get('start_rekordbox', True) and not rekordbox_pid():
                    launch(hidden=self.view.get('hide_rekordbox', True))
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
        if event.source == 'anlz_path':
            self.meta[idx] = dict(title=event.title, artist=event.artist, key=event.key, artwork=event.artwork)
        if event.anlz_path and str(event.anlz_path) != self.requested.get(idx):
            self.open_track(idx, event.anlz_path)
            self.timelines[idx].reset()
        deck = self.decks[idx]
        if self.meta.get(idx, {}).get('title'):
            deck.song_name = self.meta[idx]['title']
        deck.update_live_bpm(event.current_bpm)
        timeline = self.timelines[idx]
        timeline.rate_hint = deck.time_scale if deck.original_bpm and deck.current_bpm else None
        timeline.beat_seconds = 60 / deck.original_bpm if deck.original_bpm else None
        if event.time_seconds is not None and event.source in ('time', 'memory', 'accessibility', 'bridge'):
            timeline.timing(TIMING.get(event.source, DEFAULT_TIMING))
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
        # A controller press is a request, not confirmation that Rekordbox acted on it.
        if not self.demo:
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
        if not self.demo:
            self.decks[idx].cues = sorted(cues, key=lambda c: (c.time_sec, c.number))
            return
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
        for _ in range(128 if self.midi else 0):
            event = self.midi.get_event()
            if event is None:
                break
            self.loop_action(*event)
        for timeline in self.timelines.values():
            timeline.controller = self.demo and bool(self.midi and self.midi.controllers)
        self.poll_cues(now := time.monotonic())
        self.poll_loading(now)
        for idx, future in list(self.jobs.items()):
            if not future.done():
                continue
            del self.jobs[idx]
            try:
                deck = future.result()
                if self.decks[idx].current_bpm:
                    deck.update_live_bpm(self.decks[idx].current_bpm)
                deck.song_name = self.meta.get(idx, {}).get('title') or deck.song_name
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
                detail = getattr(self.reader, 'status', '') or 'REKORDBOX · connected'
                self.set_status(detail + (f' · {controllers}' if controllers else ''))
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
        self.loader.shutdown(wait=False, cancel_futures=True)


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
        top.setContentsMargins(12, 7, 12, 7)
        top.setSpacing(10)
        # Deck count as one segmented switch, as on the AZ's own layout toggle.
        segment = QFrame()
        segment.setObjectName('segment')
        switch = QHBoxLayout(segment)
        switch.setContentsMargins(3, 3, 3, 3)
        switch.setSpacing(2)
        self.two = self.button('2 DECKS', lambda: self.set_count(2))
        self.four = self.button('4 DECKS', lambda: self.set_count(4))
        switch.addWidget(self.two)
        switch.addWidget(self.four)
        top.addWidget(segment)
        self.browse_button = self.button('BROWSE', self.toggle_browser)
        self.browse_button.setAccessibleName('Browse Rekordbox collection')
        top.addWidget(self.browse_button)
        top.addStretch()
        # Starts Rekordbox with Wave's bridge: shown while Wave could connect directly but isn't.
        self.connect_button = self.button('CONNECT', self.connect_rekordbox)
        self.connect_button.setObjectName('connect')
        self.connect_button.setAccessibleName('Start Rekordbox connected to Wave')
        self.connect_button.hide()
        top.addWidget(self.connect_button)
        # Shown when GitHub has a newer Wave; installing waits for the user, never during playback.
        self.update_button = self.button('UPDATE', self.offer_update)
        self.update_button.setObjectName('connect')
        self.update_button.setAccessibleName('Update Rekordbox Wave')
        self.update_button.hide()
        top.addWidget(self.update_button)
        self.update = self.update_job = None
        self.update_progress = 0
        self.updates = ThreadPoolExecutor(max_workers=1, thread_name_prefix='updates')
        self.update_watch = QTimer(self)
        self.update_watch.timeout.connect(self.poll_update)
        self.update_watch.start(500)
        self.update_check = QTimer(self)
        self.update_check.timeout.connect(self.check_for_update)
        self.update_check.start(6 * 3600 * 1000)
        QTimer.singleShot(5000, self.check_for_update)
        self.status = QLabel(session.status)
        self.status.setObjectName('connection')
        self.status.setMaximumWidth(510)
        top.addWidget(self.status)
        self.clock = QLabel()
        self.clock.setObjectName('clock')
        self.clock.setAccessibleName('Time')
        top.addWidget(self.clock)
        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(lambda: self.clock.setText(time.strftime('%H:%M')))
        self.clock_timer.start(1000)
        self.clock.setText(time.strftime('%H:%M'))
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
        from .qt_browser import BrowserPage
        self.browser_page = BrowserPage()
        self.browser_page.loadRequested.connect(self.session.load_track)
        self.canvas.masterTempoToggled.connect(self.session.toggle_master_tempo)
        self.session.loadStatus.connect(self.browser_page.show_load_status)
        self.pages.addWidget(self.browser_page)
        self.deck_paths = {}
        self.deck_watch = QTimer(self)
        self.deck_watch.timeout.connect(self.sync_load_targets)
        self.starting_since = None
        self.rekordbox_shown = False
        self.deck_watch.timeout.connect(self.watch_rekordbox_start)
        self.deck_watch.start(250)
        # Follows Rekordbox's own browser as the controller's knob moves it, as the AZ's screen does.
        self.mirrored = None
        self.mirror_opened = False
        self.mirror_moved = 0.0
        self.browse_watch = QTimer(self)
        self.browse_watch.timeout.connect(self.follow_rekordbox_browser)
        self.browse_watch.start(50)
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
        self.sync_nav()
        for key, action in [('F2', self.toggle_settings), ('F11', self.toggle_fullscreen), ('Escape', self.escape),
                            ('+', lambda: self.zoom(-1)), ('=', lambda: self.zoom(-1)), ('-', lambda: self.zoom(1))]:
            zoom_key = key in ('+', '=', '-')
            shortcut = QShortcut(QKeySequence(key), self.canvas if zoom_key else self)
            if zoom_key:
                shortcut.setContext(Qt.ShortcutContext.WidgetWithChildrenShortcut)
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

    def follow_rekordbox_browser(self):
        reader = self.session.reader
        browser = getattr(reader, 'browser', None) if reader and getattr(reader, 'bridged', False) else None
        now = time.monotonic()
        if browser is None:
            self.mirrored = None
            return
        key = (browser.cursor, browser.focus, browser.tree_path, browser.rows, browser.first,
               tuple(ident for ident, _ in browser.tracks))
        if key != self.mirrored:
            first_look = self.mirrored is None
            # Only Rekordbox's cursor moving is the knob; its list refreshing by itself (a stick mounting,
            # a playlist updating) must not pull Wave off what the user picked, so it only redraws a mirror.
            # Focus counts only when it lands somewhere: on Windows it reads as none whenever Wave is touched.
            moved = not first_look and (key[0] != self.mirrored[0] or key[2] != self.mirrored[2]
                                        or (key[1] and key[1] != self.mirrored[1]))
            self.mirrored = key
            if not moved and not first_look:
                if self.pages.currentWidget() is self.browser_page and self.browser_page.view == ('rekordbox',):
                    self.browser_page.mirror(browser, [root for _, root in getattr(reader, 'devices', ())])
                return
            if moved:  # the knob moved: show it, opening the browser if it was closed
                self.mirror_moved = now
                if self.pages.currentWidget() is not self.browser_page:
                    self.toggle_browser()
                    self.mirror_opened = True
            if self.pages.currentWidget() is self.browser_page and (not first_look or self.browser_page.view == ('rekordbox',)):
                self.browser_page.mirror(browser, [root for _, root in getattr(reader, 'devices', ())])
        elif self.mirror_opened and now - self.mirror_moved > 10:
            # Opened by the knob and left alone: back to the waveforms, as after a load on the AZ.
            self.mirror_opened = False
            if self.pages.currentWidget() is self.browser_page and self.browser_page.view == ('rekordbox',):
                self.toggle_browser()

    def sync_load_targets(self):
        reader = self.session.reader
        self.connect_button.setVisible(bool(reader and hasattr(reader, 'load') and not reader.bridged))
        paths, before = getattr(reader, 'deck_paths', {}), self.deck_paths
        self.deck_paths = paths
        if self.pages.currentWidget() is self.browser_page and track_loaded(before, paths):
            self.mirror_opened = False
            self.toggle_browser()  # a track landed on a deck, from the screen or the controller: back to the waveforms
            return
        if self.pages.currentWidget() is self.browser_page:
            locked = {i: f'Deck {i + 1} is playing: stop it before loading over it' if self.session.deck_playing(i) else ''
                      for i in range(4)}
            self.browser_page.set_decks(self.session.order(), locked, self.session.load_unavailable(),
                                        getattr(reader, 'deck_paths', {}))

    def watch_rekordbox_start(self, now=None):
        """Rekordbox behind Wave can stop at its own questions (library sync, My Tag and so on) when it starts
        or loads, or hang on a music drive that stopped answering: bring it forward so it can be seen, then come back."""
        from .mac_bridge import BUSY, STARTING_TEXT, show
        status = getattr(self.session.reader, 'status', '') or ''
        now = time.monotonic() if now is None else now
        if status not in (STARTING_TEXT, BUSY):
            self.starting_since = None
            if self.rekordbox_shown and status == 'REKORDBOX · direct':
                self.rekordbox_shown = False
                self.raise_()
                self.activateWindow()
            return
        self.starting_since = self.starting_since or now
        if not self.rekordbox_shown and now - self.starting_since > 8:
            self.rekordbox_shown = True
            try:
                show()
            except Exception:
                return  # Rekordbox went away; the status says what to do
            self.update_status('Rekordbox is waiting: answer its questions, then Wave connects' if status == STARTING_TEXT else
                               'Rekordbox is not responding: answer it if it asks something, or check your music drive')

    def check_for_update(self, asked=False):
        if self.update_job or not asked and (self.session.demo or not self.session.view.get('check_updates', True)):
            return
        from . import updater
        self.update_job = ('asked' if asked else 'check', self.updates.submit(updater.check))

    def offer_update(self):
        from . import updater
        update = self.update
        if not update or self.update_job:
            return
        if not updater.can_install():  # running from source: the release page has it
            QDesktopServices.openUrl(QUrl(update.page))
            return
        if any(self.session.deck_playing(i) for i in range(4)):
            QMessageBox.information(self, 'Update Wave', 'Stop every deck first: Wave restarts to update.')
            return
        if QMessageBox.question(self, 'Update Wave', f'Update Rekordbox Wave to {update.version}?\n\n'
                                'Wave downloads it from GitHub, checks it and restarts. '
                                'Rekordbox keeps running and Wave reconnects to it.') != QMessageBox.StandardButton.Yes:
            return
        import tempfile
        folder = tempfile.mkdtemp(prefix='wave-update-')
        self.update_progress = 0
        self.update_button.setEnabled(False)

        def run():
            def progress(done, total):
                self.update_progress = done * 100 // max(total, 1)
            updater.install(updater.download(update, folder, progress))
        self.update_job = ('install', self.updates.submit(run))

    def poll_update(self):
        if not self.update_job:
            return
        kind, job = self.update_job
        if not job.done():
            if kind == 'install':
                self.update_button.setText(f'UPDATING {self.update_progress}%')
            return
        self.update_job = None
        try:
            result = job.result()
        except Exception as exc:
            if kind == 'install':
                self.update_button.setEnabled(True)
                self.update_button.setText(f'UPDATE {self.update.version}')
                QMessageBox.information(self, 'Update Wave', f'Wave did not update, and is unchanged: {exc}')
            elif kind == 'asked':
                QMessageBox.information(self, 'Check for updates', f'GitHub could not be reached: {exc}')
            return  # a check that fails (offline, GitHub busy) is tried again later, quietly
        if kind in ('check', 'asked'):
            self.update = result
            self.update_button.setVisible(bool(result))
            if result:
                self.update_button.setText(f'UPDATE {result.version}')
                self.update_button.setToolTip(f'Rekordbox Wave {result.version} is on GitHub')
            if kind == 'asked':
                from . import __version__
                QMessageBox.information(self, 'Check for updates', f'Rekordbox Wave {result.version} is ready: press UPDATE at the top.'
                                        if result else f'Rekordbox Wave {__version__} is the latest version.')
        else:
            self.update_button.setText('RESTARTING…')
            self.close()
            QApplication.quit()

    def show_rekordbox(self):
        from .mac_bridge import show
        try:
            show()
        except Exception as exc:
            QMessageBox.information(self, 'Rekordbox', str(exc))

    def connect_rekordbox(self):
        from .mac_bridge import rekordbox_pid, restart
        if rekordbox_pid() and QMessageBox.question(
                self, 'Connect Rekordbox', 'Restart Rekordbox so Wave can connect to it?\n\n'
                'Playback stops while it restarts. Rekordbox asks first if a deck is playing.') != QMessageBox.StandardButton.Yes:
            return
        self.update_status('Restarting Rekordbox with Wave…' if rekordbox_pid() else 'Starting Rekordbox with Wave…')
        job = self.session.loader.submit(restart, self.session.view.get('hide_rekordbox', True))
        def finished():
            if job.done():
                self.connect_watch.stop()
                if job.exception():
                    QMessageBox.information(self, 'Connect Rekordbox', str(job.exception()))
        self.connect_watch = QTimer(self)
        self.connect_watch.timeout.connect(finished)
        self.connect_watch.start(200)

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
            QFrame#toolbar { background:#0e0f12; border-bottom:1px solid #1d2025; }
            QLabel#connection { color:#8d96a0; font-size:13px; background:transparent; padding:0 6px; }
            QLabel#clock { color:#f2f4f6; font-size:19px; font-weight:600; background:transparent; padding:0 10px; }
            QPushButton { border:1px solid #2a2e35; border-radius:9px; background:#1a1d22; color:#e8eaed;
                          padding:8px 18px; font-size:15px; font-weight:600; min-height:28px; }
            QPushButton:hover { background:#22262c; }
            QPushButton:pressed { background:#2c3138; }
            QPushButton:disabled { color:#565c64; background:#141619; border-color:#1f2227; }
            QPushButton:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border:1px solid #1a79eb; }
            QPushButton[selected="true"] { color:#ffffff; background:#1a79eb; border-color:#1a79eb; }
            QFrame#segment { background:#1a1d22; border:1px solid #2a2e35; border-radius:11px; }
            QFrame#segment QPushButton { border:0; border-radius:8px; background:transparent; padding:6px 16px; color:#a7afb8; }
            QFrame#segment QPushButton[selected="true"] { background:#1a79eb; color:#ffffff; }
            QScrollArea { border:0; }
            QWidget#settings { background:#0b0c0e; }
            QWidget#settings QLabel { background:transparent; }
            QLabel#pageTitle { color:#ffffff; font-size:30px; font-weight:700; }
            QComboBox, QDoubleSpinBox, QSpinBox { background:#1a1d22; color:#e8eaed; padding:9px 14px; border:1px solid #2a2e35;
                                                  border-radius:9px; min-height:30px; }
            QComboBox::drop-down { border:0; width:30px; }
            QComboBox QAbstractItemView { background:#1a1d22; border:1px solid #2a2e35; selection-background-color:#1a79eb; outline:0; }
            QCheckBox { spacing:14px; padding:8px 0; background:transparent; }
            QListWidget#settingsMenu { background:transparent; border:0; outline:0; }
            QListWidget#settingsMenu::item { color:#9aa3ad; padding:16px 20px; margin:3px 0; border-radius:10px; font-weight:600; }
            QListWidget#settingsMenu::item:hover { background:#181b20; color:#e8eaed; }
            QListWidget#settingsMenu::item:selected { background:#1a79eb; color:#ffffff; }
            QStackedWidget#settingsPages, QStackedWidget#settingsPages QScrollArea, QStackedWidget#settingsPages QScrollArea > QWidget > QWidget {
                background:#14161a; border-radius:14px; }
            QCheckBox::indicator { width:26px; height:26px; border-radius:7px; }
            QCheckBox::indicator:unchecked { background:#1a1d22; border:1px solid #3a3f46; }
            QCheckBox::indicator:checked { background:#1a79eb; border:1px solid #1a79eb; image:none; }
            QLabel#section { color:#fff; font-size:25px; font-weight:600; padding-top:14px; }
            QLabel#help { color:#8d96a0; font-size:16px; }
            QScrollBar:vertical { background:transparent; width:10px; }
            QScrollBar::handle:vertical { background:#3a3f46; min-height:40px; border-radius:5px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
            QWidget#browser { background:#0a0a0a; }
            QWidget#browser QFrame#browserNav { background:#0d0d0d; border-right:1px solid #000; }
            QWidget#browser QFrame#browserTop { background:#141414; border-bottom:1px solid #1d1d1d; }
            QWidget#browser QLabel#heading { color:#b8b8b8; font-size:22px; background:transparent; }
            QWidget#browser QPushButton#back { font-size:34px; padding:0 0 6px 0; border:1px solid #2c2c2c; border-radius:4px; background:#1b1b1b; }
            QWidget#browser QPushButton#back:disabled { color:#3a3a3a; border-color:#1d1d1d; background:#141414; }
            QWidget#browser QPushButton#keys { font-size:17px; font-weight:600; padding:0; border:1px solid #2c2c2c; border-radius:4px; background:#1b1b1b; }
            QWidget#browser QLineEdit { background:#1f1f1f; color:#fff; border:1px solid #333; border-radius:4px; padding:0 14px; font-size:21px; selection-background-color:#1a79eb; }
            QWidget#browser QLineEdit:focus { border:1px solid #1a79eb; }
            QWidget#browser QTableView, QWidget#browser QListView { background:#0a0a0a; border:0; outline:0; }
            QWidget#browser QHeaderView::section { background:#141414; color:#8a8a8a; border:0; border-bottom:1px solid #262626; padding:6px; font-size:14px; font-weight:600; }
            QWidget#browser QLabel#message { background:#0d0d0d; color:#9aa3ad; font-size:15px; padding:6px 16px; border-top:1px solid #1d1d1d; }
            QWidget#browser QFrame#loadbar { background:#141414; border-top:1px solid #262626; }
            QWidget#browser QWidget#sourcesPage { background:#0a0a0a; }
            QWidget#browser QWidget#keyboard { background:#111; border-top:1px solid #262626; }
            QWidget#browser QWidget#keyboard QPushButton { background:#222; border:1px solid #333; border-radius:4px; font-size:19px; }
            QWidget#browser QWidget#keyboard QPushButton:pressed { background:#1a79eb; }
            QWidget#browser QScrollBar:vertical { width:10px; background:#0a0a0a; }
            QWidget#browser QScrollBar::handle:vertical { background:#3a3a3a; min-height:40px; border-radius:5px; }
            QPushButton#connect { color:#000; background:#ff9f1a; border-color:#ff9f1a; }
        ''' + f'QCheckBox::indicator:checked {{ image:url("{check_icon}"); }}\nQWidget#settings, QWidget#settings * {{ font-size:{size}px; }}\nQWidget#settings QPushButton {{ min-height:30px; }}\nQWidget#settings QLabel#pageTitle {{ font-size:{round(size * 1.45)}px; font-weight:700; }}')

    def build_settings(self):
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        content.setObjectName('settings')
        outer = QVBoxLayout(content)
        outer.setContentsMargins(34, 24, 34, 34)
        outer.setSpacing(18)
        first = QHBoxLayout()
        heading = QLabel('Settings')
        heading.setObjectName('pageTitle')
        first.addWidget(heading)
        first.addStretch()
        first.addWidget(self.button('DONE', self.toggle_settings))
        outer.addLayout(first)
        # A side menu beside its page, as on the AZ, rather than a row of tabs.
        body = QHBoxLayout()
        body.setSpacing(18)
        menu = QListWidget()
        menu.setObjectName('settingsMenu')
        menu.setAccessibleName('Settings sections')
        menu.setFixedWidth(230)
        menu.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        pages = QStackedWidget()
        pages.setObjectName('settingsPages')
        menu.currentRowChanged.connect(pages.setCurrentIndex)
        body.addWidget(menu)
        body.addWidget(pages, 1)
        outer.addLayout(body, 1)
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
            pages.addWidget(area)
            menu.addItem(title)
            menu.setCurrentRow(0)
            return form
        display_form = tab('DISPLAY')
        waveform_form = tab('WAVEFORM')
        info_form = tab('DECK INFO')
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
        for label, key in [('Beat and bar markers', 'grid'),
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
        form = info_form
        def choice(label, key, options):
            combo = QComboBox()
            combo.setAccessibleName(label)
            for text, value in options:
                combo.addItem(text, value)
            combo.setCurrentIndex(max(0, combo.findData(self.session.view.get(key))))
            combo.currentIndexChanged.connect(lambda _, c=combo, k=key: self.set_view(k, c.currentData()))
            form.addRow(label, combo)
        choice('Time display', 'time_mode', [('Remaining · -03:27.1', 'remain'), ('Elapsed · 01:12.4', 'elapsed')])
        choice('Information bar size', 'info_size', [('Compact', .8), ('Standard', 1.0), ('Large', 1.25)])
        choice('End-of-track warning', 'end_warning', [('Off', 0), ('Last 15 seconds', 15), ('Last 30 seconds', 30),
                                                        ('Last 60 seconds', 60)])
        for label, key in [('Show the information bar', 'deck_info'), ('Show artwork', 'artwork')]:
            box = QCheckBox(label)
            box.setChecked(bool(self.session.view.get(key, True)))
            box.toggled.connect(lambda value, k=key: self.set_view(k, value))
            form.addRow('', box)
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
        if sys.platform in ('darwin', 'win32'):
            state_help = QLabel('Wave reads decks, loops and tempo straight from Rekordbox, which it starts with its bridge. '
                                'Rekordbox stays the audio engine and keeps the FLX10.')
            state_help.setWordWrap(True)
            state_help.setObjectName('help')
            form.addRow('Rekordbox', state_help)
            for label, key in [('Start Rekordbox with Wave', 'start_rekordbox'), ('Keep Rekordbox hidden behind Wave', 'hide_rekordbox'),
                               ('Check GitHub for Wave updates', 'check_updates')]:
                box = QCheckBox(label)
                box.setChecked(bool(self.session.view.get(key, True)))
                box.toggled.connect(lambda value, k=key: self.set_view(k, value))
                form.addRow('', box)
            rekordbox_row = QWidget()
            rekordbox_layout = QHBoxLayout(rekordbox_row)
            rekordbox_layout.setContentsMargins(0, 0, 0, 0)
            rekordbox_layout.addWidget(self.button('SHOW REKORDBOX', self.show_rekordbox))
            rekordbox_layout.addWidget(self.button('CHECK FOR UPDATES', lambda: self.check_for_update(asked=True)))
            if sys.platform == 'darwin':
                rekordbox_layout.addWidget(self.button('ACCESSIBILITY SETTINGS', lambda:
                    QDesktopServices.openUrl(QUrl('x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility'))))
            rekordbox_layout.addStretch()
            form.addRow('', rekordbox_row)
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
        opening = self.pages.currentWidget() is not self.settings_page
        self.pages.setCurrentWidget(self.settings_page if opening else self.canvas)
        self.sync_nav()
        if opening:
            self.refresh_screens()
        else:
            self.canvas.setFocus()
            self.pace_frames()

    def toggle_browser(self):
        opening = self.pages.currentWidget() is not self.browser_page
        self.pages.setCurrentWidget(self.browser_page if opening else self.canvas)
        self.sync_nav()
        if opening:
            self.browser_page.search.setFocus()
        else:
            self.pace_frames()

    def escape(self):
        if self.pages.currentWidget() is self.browser_page:
            if self.browser_page.keyboard.isVisible():
                self.browser_page.keyboard.hide()
            elif not self.browser_page.go_back():
                self.toggle_browser()
        elif self.pages.currentIndex():
            self.toggle_settings()
        elif self.isFullScreen():
            self.toggle_fullscreen()

    def mark(self, button, on):
        button.setProperty('selected', on)
        button.style().unpolish(button)
        button.style().polish(button)

    def sync_count(self):
        self.mark(self.two, self.session.render.deck_count == 2)
        self.mark(self.four, self.session.render.deck_count == 4)

    def sync_nav(self):
        """One visual language for the toolbar: the active view glows, like the deck buttons."""
        current = self.pages.currentWidget()
        self.mark(self.browse_button, current is self.browser_page)
        self.mark(self.settings_button, current is self.settings_page)

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
            self.sync_nav()
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
        self.deck_watch.stop()
        self.browse_watch.stop()
        self.browser_page.shutdown()
        self.update_watch.stop()
        self.updates.shutdown(wait=False, cancel_futures=True)
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
    from .updater import clean_up
    try:
        clean_up()
    except OSError:
        pass  # a leftover of an earlier update; it is tried again next start
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
