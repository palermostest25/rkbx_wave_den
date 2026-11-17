"""One native window: waveform display and integrated, readable settings."""
from __future__ import annotations

import argparse
import json
import logging
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

from . import __version__
from .config import WaveformColorConfig, WaveformRenderConfig, config_to_dict, dict_to_config
from .cues import CueMarker, _marker_color, live_cues
from .deck_controller import DeckController
from .displays import deck_order
from .catalog import CatalogTrack, tracks_for_paths
from .live_state import PlaybackTimeline
from .qt_display import WaveformCanvas, screen_key, find_screen, bind_fullscreen
from .mac_bridge import BUSY, STARTING_TEXT

log = logging.getLogger(__name__)
SETTINGS_VERSION = 2


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
    PREVIEW_HOLD = 3.0    # seconds a touch preview stays after the finger lifts
    PREVIEW_RETURN = .35  # and the glide back to the playhead
    changed = Signal()
    zoomRequested = Signal(float)  # multiply the visible beats
    knobZoom = Signal(int)         # the sampler knob's position, 0-127: sets the zoom
    analysisChanged = Signal()
    statusChanged = Signal(str)
    loadStatus = Signal(str)
    controllerBrowse = Signal(str, int, int)  # 'browse' (steps) | 'browse_press' | 'back' | 'load' (deck), from MIDI

    def __init__(self, *, demo=False, config_path=None):
        super().__init__()
        self.path = Path(config_path) if config_path else config_directory() / 'default_config.json'
        self.colors = WaveformColorConfig(**STYLES['Rekordbox'])
        self.render = WaveformRenderConfig()
        self.view = dict(zoom_beats=32, playhead=.32, deck_info=True, time_mode='remain', info_size=1.0,
                         artwork=True, end_warning=30, start_rekordbox=True, hide_rekordbox=True, check_updates=True,
                         grid=True, cues=True, loops=True, bar_numbers=True,
                         source='Native', settings_scale=1.0, always_on_top=True,
                         refresh_hz=60, style='Rekordbox', browser='wave', knob_loads_wave=True,
                         auto_connect=True, keep_rekordbox_running=True, quit_rekordbox=True,
                         close_popups=True, keep_awake=True, settings_version=SETTINGS_VERSION, tap_to_open='double', sampler_zoom=True, sampler_zoom_reverse=False, touch_keyboard_off=True, touch_keyboard_restore=None,
                         dim=1.0, battery=True, master_bpm=True)
        self.decks = {i: DeckController() for i in range(4)}
        self.previews = {}    # deck -> (seconds, held until) while its overview is touched
        self.knob_beats = None  # the sampler knob's zoom, while it is in use
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
        self.wave_loads = {}  # deck -> (file Wave asked for, until when): Wave's own loads, not the controller's
        self.vsync_wait = None
        self.host = None      # keeps Rekordbox started and connected behind Wave (Native)
        self.popups = None    # watches Rekordbox's popups (Native)
        self.next_host = 0.0
        self.facts = {}       # pid -> (age at, started, bridge libraries) for the host
        self.status = ''
        self.load_config(self.path, initial=True)
        if demo:
            from .demo import populate_demo
            populate_demo(self.decks)
            self.started = time.monotonic()
            self.set_status('DEMO · synthetic analysis')
        else:
            self.connect_source()
            from .midi_loops import MidiLoopListener, shared_midi
            if shared_midi():
                # The browse knob, BACK and LOAD, and SHIFT+BROWSE zoom, read beside Rekordbox. Only where the
                # system shares the controller: classic Windows MIDI gives a port to one app, and taking the
                # FLX10 from Rekordbox would stop its decks.
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
        self.wave_loads[idx] = (expected, time.monotonic() + 15)
        self.loadStatus.emit(f'Loading {track.title} on deck {idx + 1}…')
        return True

    def loaded_by_wave(self, idx, path):
        """True when deck ``idx`` got ``path`` because Wave asked for it (not the controller's LOAD)."""
        expected, until = self.wave_loads.get(idx, ('', 0))
        if time.monotonic() > until or not expected or not path:
            return bool(self.loading and self.loading[0] == idx)
        a, b = Path(path).as_posix().casefold(), Path(expected).as_posix().casefold()
        return a == b or a.endswith(b) or b.endswith(a)

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
            saved_view = data.get('view', {})
            view.update(saved_view)
            if int(saved_view.get('settings_version', 1) or 1) < 2:
                # 2.5: Rekordbox's own browser is BROWSE, as on the AZ; Wave's stays in Settings.
                view['browser'] = 'rekordbox'
            view['settings_version'] = SETTINGS_VERSION
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
        if self.host:
            self.host.close()
            self.host = None
        if self.popups:
            self.popups.stop()
            self.popups = None
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
                from .mac_bridge import RekordboxBridgeReader
                from .popups import PopupWatcher
                from .rekordbox_host import RekordboxHost
                self.reader = RekordboxBridgeReader(deck_count=4)
                # Rekordbox is the engine behind Wave: started, connected and kept running out of sight.
                self.host = RekordboxHost(hidden=lambda: self.view.get('hide_rekordbox', True))
                self.configure_host()
                self.popups = PopupWatcher()
                self.popups.auto_close = bool(self.view.get('close_popups', True))
                self.popups.start()
            else:
                raise RuntimeError('Native Rekordbox connection requires Windows or macOS')
            self.reader.start()
            self.set_status('Waiting for Rekordbox')
        except Exception as exc:
            self.reader = None
            self.reader_error = str(exc)
            self.set_status(self.reader_error)

    def configure_host(self):
        if self.host:
            policy = self.host.policy
            policy.auto_start = bool(self.view.get('start_rekordbox', True))
            policy.auto_connect = bool(self.view.get('auto_connect', True))
            policy.keep_running = bool(self.view.get('keep_rekordbox_running', True))
        if self.popups:
            self.popups.auto_close = bool(self.view.get('close_popups', True))

    def seen(self):
        """What Wave sees of Rekordbox, for the host."""
        from .rekordbox_host import Seen
        from .mac_bridge import bridge_modules, process_age
        reader = self.reader
        pid = getattr(reader, 'pid', None)
        state = getattr(reader, 'state', 'absent')
        if pid and pid not in self.facts:
            self.facts = {pid: dict(age=process_age(pid), at=time.monotonic(), modules=None)}
        facts = self.facts.get(pid, {})
        age = facts['age'] + time.monotonic() - facts['at'] if pid and facts.get('age') is not None else None
        if pid and state == 'none' and sys.platform == 'win32' and facts.get('modules') is None:
            facts['modules'] = bridge_modules(pid)
        playing = any(t.playing for t in self.timelines.values()) or any(
            d.playing for d in getattr(reader, 'deck_status', {}).values())
        loaded = bool(self.requested) or any(getattr(reader, 'deck_paths', {}).values())
        return Seen(pid, state, bool(getattr(reader, 'stale', False)), playing, loaded,
                    bool(getattr(reader, 'connected', False)), age, bool(facts.get('modules')))

    def poll_host(self, now):
        if not self.host or now < self.next_host or not getattr(self.reader, 'checked', True):
            return  # not before the reader has looked for Rekordbox once
        self.next_host = now + .5
        self.host.poll(self.seen(), now)
        if self.popups:
            self.popups.pid = getattr(self.reader, 'pid', None)

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
        if kind == 'browse':
            value = beats[1]
            steps = value if value < 64 else value - 128
            if steps:
                self.controllerBrowse.emit('browse', steps, -1)
            return
        if kind in ('browse_press', 'back', 'view', 'load'):
            self.controllerBrowse.emit(kind, 0, -1 if idx is None else idx)
            return
        if kind == 'sampler_zoom':
            self.knobZoom.emit(int(beats[1]))
            return
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

    # The sampler knob as a zoom: its middle is the zoom set in Settings, its ends 1 bar and 32 bars.
    KNOB_NEAR, KNOB_FAR = 4.0, 128.0  # beats across the lane at the knob's ends

    def zoom_beats(self):
        """Beats across a lane now: the sampler knob's zoom while it is in use, else the zoom setting."""
        return self.knob_beats or self.view['zoom_beats']

    def knob_zoom(self, value, reverse=False):
        """Map the knob (0-127, 64 the middle) onto 1-32 bars, through the zoom setting at the middle."""
        base = min(self.KNOB_FAR, max(self.KNOB_NEAR, float(self.view['zoom_beats'])))
        x = max(-1.0, min(1.0, (value - 64) / (63 if value > 64 else 64)))  # 0 and 127 both reach the ends
        if not reverse:
            x = -x  # clockwise zooms in: fewer beats, a bigger waveform
        end = self.KNOB_FAR if x > 0 else self.KNOB_NEAR
        self.knob_beats = base * (end / base) ** abs(x)  # even steps in octaves, each side
        return self.knob_beats

    def preview_hold(self, idx, seconds):
        """A finger on deck ``idx``'s overview: its lane shows ``seconds`` until the finger lifts."""
        self.previews[idx] = (seconds, None)

    def preview_release(self, idx):
        if idx in self.previews:
            self.previews[idx] = (self.previews[idx][0], time.monotonic() + self.PREVIEW_HOLD)

    def preview_position(self, idx, now=None):
        """Where deck ``idx``'s lane looks while previewing, or None. It eases back to the playhead."""
        look = self.previews.get(idx)
        if look is None:
            return None
        seconds, until = look
        now = time.monotonic() if now is None else now
        if until is None:
            return seconds
        live = self.position(idx)
        if live is None or now >= until + self.PREVIEW_RETURN:
            self.previews.pop(idx, None)
            return None
        if now <= until:
            return seconds
        blend = (now - until) / self.PREVIEW_RETURN
        blend = blend * blend * (3 - 2 * blend)  # smooth glide home
        return seconds + (live - seconds) * blend

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
        try:
            self.poll_host(now)
        except Exception:
            log.exception('Rekordbox host check failed')
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
            doing = self.host.doing if self.host else ''
            if doing:
                self.set_status(f'{doing}…')
            elif self.reader.connected:
                controllers = ' · '.join(self.midi.controllers or [self.midi.status]) if self.midi else ''
                detail = getattr(self.reader, 'status', '') or 'REKORDBOX · connected'
                self.set_status(detail + (f' · {controllers}' if controllers else ''))
            elif self.host and self.host.error and getattr(self.reader, 'state', '') in ('absent', 'none'):
                self.set_status(self.host.error)
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
        if self.popups:
            self.popups.stop()
        if self.host:
            self.host.close()
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
        from .touch_keyboard import Guard
        self.touch = Guard(session.view, self.persist)  # before Settings, which offers its own control
        self.setWindowTitle(f'Rekordbox Wave {__version__}')
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
        self.browse_button = self.button('BROWSE', self.browse)
        self.browse_button.setAccessibleName('Browse Rekordbox collection')
        top.addWidget(self.browse_button)
        # Rekordbox itself, for anything only it can do: Wave steps aside; tap Wave to come back.
        self.rekordbox_button = self.button('REKORDBOX', self.show_rekordbox)
        self.rekordbox_button.setAccessibleName('Show Rekordbox')
        self.rekordbox_button.setToolTip('Show Rekordbox in front of Wave. Tap Wave to come back.')
        self.rekordbox_button.setVisible(sys.platform in ('win32', 'darwin') and not session.demo)
        top.addWidget(self.rekordbox_button)
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
        self.updating = False   # quitting to install an update: Rekordbox keeps running
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
        self.status.setTextFormat(Qt.TextFormat.RichText)
        self.status.setMaximumWidth(510)
        top.addWidget(self.status)
        self.master_label = QLabel()
        self.master_label.setObjectName('master')
        self.master_label.setAccessibleName('Tempo master')
        top.addWidget(self.master_label)
        self.battery_label = QLabel()
        self.battery_label.setObjectName('battery')
        self.battery_label.setAccessibleName('Battery')
        top.addWidget(self.battery_label)
        self.battery_plugged = None
        self.clock = QLabel()
        self.clock.setObjectName('clock')
        self.clock.setAccessibleName('Time')
        top.addWidget(self.clock)
        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(lambda: self.clock.setText(time.strftime('%H:%M')))
        self.clock_timer.timeout.connect(self.update_toolbar_readouts)
        self.clock_timer.start(1000)
        self.clock.setText(time.strftime('%H:%M'))
        self.settings_button = self.button('SETTINGS', self.toggle_settings)
        self.settings_button.setAccessibleName('Open display settings')
        top.addWidget(self.settings_button)
        self.layout.addWidget(self.toolbar)
        # One line under the toolbar for what the DJ should know now: Rekordbox wants an answer,
        # a notice Wave closed, a step to take. Never a modal box during a set.
        self.banner = QFrame()
        self.banner.setObjectName('banner')
        line = QHBoxLayout(self.banner)
        line.setContentsMargins(16, 6, 10, 6)
        line.setSpacing(12)
        self.banner_text = QLabel()
        self.banner_text.setObjectName('bannerText')
        self.banner_text.setWordWrap(True)
        line.addWidget(self.banner_text, 1)
        self.banner_action = self.button('', lambda: self.banner_callback and self.banner_callback())
        self.banner_action.setObjectName('bannerAction')
        line.addWidget(self.banner_action)
        self.banner_close = self.button('✕', lambda: self.hide_banner())
        self.banner_close.setObjectName('bannerClose')
        self.banner_close.setAccessibleName('Dismiss message')
        line.addWidget(self.banner_close)
        self.banner_callback = None
        self.banner_key = None
        self.banner_timer = QTimer(self)
        self.banner_timer.setSingleShot(True)
        self.banner_timer.timeout.connect(lambda: self.hide_banner())
        self.banner.hide()
        self.layout.addWidget(self.banner)
        self.pages = QStackedWidget()
        self.canvas = WaveformCanvas(session)
        self.canvas.settingsRequested.connect(self.toggle_settings)
        self.canvas.zoomBy.connect(self.zoom_by)
        session.zoomRequested.connect(self.zoom_by)
        session.knobZoom.connect(self.on_knob_zoom)
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
        self.browser_page.open_on = session.view.get('tap_to_open', 'double')
        self.browser_page.loadRequested.connect(self.session.load_track)
        self.canvas.masterTempoToggled.connect(self.session.toggle_master_tempo)
        self.session.controllerBrowse.connect(self.on_controller)
        self.canvas.timeModeToggled.connect(
            lambda: self.set_view('time_mode', 'elapsed' if self.session.view.get('time_mode') == 'remain' else 'remain'))
        self.session.loadStatus.connect(self.browser_page.show_load_status)
        self.pages.addWidget(self.browser_page)
        self.deck_paths = {}
        self.deck_watch = QTimer(self)
        self.deck_watch.timeout.connect(self.sync_load_targets)
        self.stuck_since = None
        self.aside = None         # why Wave stepped aside for Rekordbox, while it has
        self.presented = []       # Rekordbox windows Wave put over itself (Windows)
        self.closed_seen = 0.0    # the newest popup Wave closed that the banner has told
        self.told_popups = False  # told the DJ how to let Wave read Rekordbox's popups (macOS)
        # While Rekordbox's own browser is up: dict(kind='mac'|'window'|'whole', tracks=deck tracks when it
        # opened, hwnd=its window and main=whether that is Rekordbox's main window (Windows), state=Wave's).
        self.rekordbox_browsing = None
        self.split_asked = False
        self.deck_watch.timeout.connect(lambda: self.watch_rekordbox())
        self.deck_watch.start(250)
        # Follows Rekordbox's own browser as the controller's knob moves it, as the AZ's screen does.
        self.mirrored = None
        self.knob_focus = ''          # the last pane Rekordbox's knob was in: 'list' or 'tree'
        self.controller_seen = False  # the controller's own knob messages have arrived (MIDI)
        self.pressed_at = 0.0         # when the controller's knob was last pushed (MIDI)
        self.knob_logged = False
        self.rekordbox_cursor = ''    # the file under Rekordbox's cursor: what controller LOAD loads
        self.redirect = None          # (deck, file) while Wave swaps a controller load for its own pick
        self.browse_watch = QTimer(self)
        self.browse_watch.timeout.connect(self.follow_rekordbox_browser)
        self.browse_watch.start(50)
        self.layout.addWidget(self.pages, 1)
        from .qt_display import Dimmer
        self.dimmer = Dimmer(self.root, lambda: float(self.session.view.get('dim', 1.0) or 1.0))
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
        from .gig import KeepAwake
        self.awake = KeepAwake()
        if not session.demo:
            self.awake.set(bool(session.view.get('keep_awake', True)))
            self.touch.start()  # Windows' touch keyboard off while Wave runs: Wave has its own
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
        """The controller's browse knob, seen through Rekordbox's own browser, for when Wave can't read the
        controller itself. Only while Wave's touch browser is open (VIEW or BROWSE opens it): a cursor move in
        Rekordbox's list moves Wave's highlight; Rekordbox moving to another playlist highlights that one."""
        reader = self.session.reader
        browser = getattr(reader, 'browser', None) if reader and getattr(reader, 'bridged', False) else None
        if browser is None:
            self.mirrored = None
            return
        ids = tuple(ident for ident, _ in browser.tracks)
        key = (browser.cursor, browser.focus, browser.tree_path, browser.rows, browser.first, ids)
        self.page_devices()
        if key == self.mirrored:
            return
        previous, self.mirrored = self.mirrored, key
        self.rekordbox_cursor = self.cursor_track(browser)
        if previous is None:
            return  # the first look is where Rekordbox was, not a move
        cursor_moved = key[0] != previous[0] and key[0] >= 0 and previous[0] >= 0
        list_changed = key[3] != previous[3] or key[2] != previous[2] or (
            not cursor_moved and key[4] == previous[4] and ids and previous[5] and not set(ids) & set(previous[5]))
        if not cursor_moved and not list_changed:
            return  # Rekordbox's rows refreshed by themselves (a stick mounting, a playlist updating)
        if self.session.view.get('browser') == 'rekordbox' or self.controller_seen:
            return  # Rekordbox's browser follows its own knob; or the controller's messages drive Wave's
        page = self.browser_page
        if self.pages.currentWidget() is not page:
            return  # the knob only browses once the browser is open (VIEW or BROWSE opens it)
        if cursor_moved:
            if not self.knob_logged:
                self.knob_logged = True
                log.info("The browse knob is followed through Rekordbox's browser")
            page.step(key[0] - previous[0])  # whatever Wave shows: playlists or tracks
        elif list_changed and page.stack.currentWidget() is page.list:
            self.locate_rekordbox(browser)  # the knob in Rekordbox's tree: highlight the same one

    def locate_rekordbox(self, browser):
        page = self.browser_page
        paths = [path for _, path in browser.tracks if path]
        source = page.source_for(CatalogTrack('', '', '', 0.0, '', 0.0, paths[0])) if paths else ''

        def found(located):
            if located is None or self.pages.currentWidget() is not page:
                return
            if source != page.source_path:
                return  # Rekordbox is on another library than Wave's browser: leave Wave where the DJ put it
            page.highlight_item(located.identifier)
        from .catalog import locate
        page.run(locate, found, source, [ident for ident, _ in browser.tracks], browser.rows)

    def page_devices(self):
        roots = tuple(root for _, root in getattr(self.session.reader, 'devices', ()))
        if roots != self.browser_page.device_roots:
            self.browser_page.device_roots = roots

    def on_controller(self, kind, steps, deck):
        """The controller's browse knob, its push, BACK and LOAD, read straight from the controller."""
        if kind == 'browse_press':
            self.pressed_at = time.monotonic()  # a load that follows is the same push, not a second one
        if not self.controller_seen:
            self.controller_seen = True  # from now on the controller's own messages drive the browser
            log.info('The browse knob is read from the controller itself (%s)', kind)
        if kind == 'view':
            self.browse(by_knob=True)  # VIEW opens the browser, and closes it again
            return
        if self.session.view.get('browser') == 'rekordbox' or kind == 'load':
            return  # Rekordbox's own browser follows its controller; LOAD is seen when the deck changes
        page = self.browser_page
        if self.pages.currentWidget() is not page:
            return  # the knob only browses once the browser is open
        if kind == 'browse':
            page.step(steps)
        elif kind == 'browse_press':
            page.enter()
        elif kind == 'back' and not page.go_back():
            self.toggle_browser()  # BACK at the top: back to the waveforms

    @staticmethod
    def cursor_track(browser):
        """The file under Rekordbox's own cursor, which the controller's LOAD loads."""
        row = browser.cursor - browser.first
        return browser.tracks[row][1] if 0 <= row < len(browser.tracks) else ''

    def sync_load_targets(self):
        reader = self.session.reader
        host = self.session.host
        self.connect_button.setVisible(bool(host.policy.ask) if host else
                                       bool(reader and hasattr(reader, 'load') and (not reader.bridged or getattr(reader, 'stale', False))))
        paths, before = getattr(reader, 'deck_paths', {}), self.deck_paths
        self.deck_paths = paths
        if self.rekordbox_browsing:
            browsing = self.rekordbox_browsing
            if track_loaded(browsing['tracks'], self.deck_tracks()):
                self.leave_rekordbox()
            elif browsing['kind'] == 'whole' and not self.isMinimized():
                self.rekordbox_browsing = None  # the user came back to Wave by hand
            elif browsing['kind'] == 'window' and not self.rekordbox_window_alive(browsing['hwnd']):
                self.leave_rekordbox()  # closed or minimised in Rekordbox
        if self.redirect and self.redirect_landed(paths):
            return
        if self.pages.currentWidget() is self.browser_page and track_loaded(before, paths):
            if self.redirect_controller_load(before, paths):
                return
            self.toggle_browser()  # a track landed on a deck, from the screen or the controller: back to the waveforms
            return
        if self.pages.currentWidget() is self.browser_page:
            locked = {i: f'Deck {i + 1} is playing: stop it before loading over it' if self.session.deck_playing(i) else ''
                      for i in range(4)}
            self.browser_page.set_decks(self.session.order(), locked, self.session.load_unavailable(),
                                        getattr(reader, 'deck_paths', {}))

    def redirect_controller_load(self, before, paths):
        """A deck changed while Wave's touch browser is open, and Wave didn't load it: that is the controller's
        LOAD, which loads Rekordbox's own pick. Load Wave's pick onto that deck instead; with nothing picked
        in Wave, put back what the deck had. True while Wave is doing either."""
        if not self.session.view.get('knob_loads_wave', True):
            return False
        page = self.browser_page
        picked = page.selected_track()
        same = lambda a, b: bool(a and b) and (Path(a).as_posix().casefold() == Path(b).as_posix().casefold()
                                               or Path(a).as_posix().casefold().endswith(Path(b).as_posix().casefold()))
        for idx, path in paths.items():
            if idx not in before or before[idx] == path or not path or self.session.loaded_by_wave(idx, path):
                continue
            if picked is not None and same(path, picked.path):
                return False  # Rekordbox loaded what Wave shows anyway
            if picked is None and page.stack.currentWidget() is page.list and \
                    time.monotonic() - self.pressed_at > 1.5:
                # The knob pushed (or LOAD pressed) on a highlighted playlist: Rekordbox took it as a load.
                # In Wave it opens the playlist; the deck gets its own track back below.
                page.enter()
            if picked is not None:
                track, source = picked, page.source_path
            elif before[idx]:
                track, source = None, ''
            else:
                page.show_load_status(f"Rekordbox loaded a track on deck {idx + 1}: Wave can't unload it. "
                                      'Highlight a track here, then LOAD.')
                return False
            if track is None:
                # Nothing picked in Wave: the deck gets back what it had before the stray LOAD.
                self.redirect = (idx, path, time.monotonic() + 10)

                def restore(found, deck=idx, old=before[idx]):
                    old_track = found.get(old)
                    if old_track and old_track.identifier:
                        self.session.load_track(deck, *page.for_loading(old_track))
                page.run(tracks_for_paths, restore, [before[idx]])
                page.show_load_status('Nothing was picked here: putting the deck back as it was.')
                log.info('Controller LOAD on deck %d with nothing picked in Wave: restoring its track', idx + 1)
                return True
            if self.session.load_track(idx, track, source):
                self.redirect = (idx, path, time.monotonic() + 10)
                page.show_load_status(f'Loading {track.title} on deck {idx + 1}, as picked here…')
                log.info('Controller LOAD on deck %d: loading the track picked in Wave instead', idx + 1)
                return True
        return False

    def redirect_landed(self, paths):
        """While a redirected load is under way, wait for it (or give up) before leaving the browser."""
        idx, rekordbox_pick, deadline = self.redirect
        if self.session.loading and time.monotonic() < deadline:
            return True
        self.redirect = None
        swapped = paths.get(idx) and paths.get(idx) != rekordbox_pick
        if swapped and self.pages.currentWidget() is self.browser_page:
            self.toggle_browser()  # Wave's pick is on the deck: back to the waveforms
        return True  # if it didn't land, the browser stays, with Rekordbox's answer in its message line

    def watch_rekordbox(self, now=None):
        """Rekordbox behind Wave can stop at its own questions (library sync, My Tag, sign-in…) when it starts or
        loads, show a popup Wave could not close for the DJ, or hang on a music drive that stopped answering. Then
        Wave steps aside and puts Rekordbox in front, and comes back once Rekordbox is running again."""
        session = self.session
        reader, watcher = session.reader, session.popups
        state = getattr(reader, 'state', '') or ''
        status = getattr(reader, 'status', '') or ''
        if not isinstance(state, str):
            state = ''
        now = time.monotonic() if now is None else now
        if watcher:
            watcher.auto_close = bool(session.view.get('close_popups', True)) and not self.rekordbox_browsing
            hwnd = self.rekordbox_browsing.get('hwnd') if self.rekordbox_browsing else None
            watcher.exclude = {hwnd} if hwnd else set()
            if watcher.error and not self.told_popups and sys.platform == 'darwin' and 'Accessibility' in watcher.error:
                self.told_popups = True
                self.show_banner("Allow Rekordbox Wave in Accessibility so it can close Rekordbox's popups for you", 'info',
                                 action=('OPEN SETTINGS', lambda: QDesktopServices.openUrl(QUrl(
                                     'x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility'))),
                                 timeout=20000, key='accessibility')
            fresh = [(t, text) for t, text in watcher.closed if t > self.closed_seen]
            if fresh:
                self.closed_seen = fresh[-1][0]
                self.show_banner(f'Closed a Rekordbox notice: {fresh[-1][1]}', 'info', timeout=6000, key='closed')
        stuck = state in ('starting', 'busy') or status in (STARTING_TEXT, BUSY)
        self.stuck_since = (self.stuck_since or now) if stuck else None
        # A popup with buttons wants an answer; one without (Rekordbox's splash) only counts once it is stuck.
        popups = [] if self.rekordbox_browsing or not watcher else \
            [p for p in watcher.popups if p.buttons and now - p.first_seen > 1.5]
        patience = 20 if state == 'starting' or status == STARTING_TEXT else 8  # a cold start takes a while
        waiting = self.stuck_since is not None and now - self.stuck_since > patience
        if popups or waiting:
            why = (f'Rekordbox asks: {popups[0].summary}' if popups else
                   'Rekordbox is waiting: answer its questions, then Wave connects'
                   if state == 'starting' or status == STARTING_TEXT else
                   'Rekordbox is not responding: answer it if it asks something, or check your music drive')
            self.step_aside(why, popups)
        elif self.aside and not stuck:
            self.come_back()
        pill = self.pill(self.session.status)
        if pill != self.status.text():
            self.status.setText(pill)

    def watch_rekordbox_start(self, now=None):
        self.watch_rekordbox(now)

    def step_aside(self, why, popups=()):
        """Put Rekordbox, or just its popups, in front of Wave until it is answered."""
        first = self.aside is None
        if why != self.aside:
            self.aside = why
            self.update_status(why)
            self.show_banner(why + ' · Wave comes back by itself', 'warn', key='aside')
        try:
            if sys.platform == 'win32':
                self.present_rekordbox(popups)
            elif first:
                from .mac_bridge import show
                self.set_on_top(False)
                show()
                log.info('Brought Rekordbox in front of Wave: %s', why)
        except Exception as exc:
            log.warning('Could not bring Rekordbox forward: %s', exc)

    def present_rekordbox(self, popups):
        """Windows: Rekordbox's popups - or, with none to see, its main window - over Wave, centred on Wave."""
        from . import win_windows as ww
        pid = getattr(self.session.reader, 'pid', None)
        if not pid:
            return
        handles = [p.key for p in popups if isinstance(p.key, int)]
        if not handles:
            main, _, shown = ww.classify(ww.windows_of(pid))
            handles = [w.hwnd for w in shown] or ([main.hwnd] if main else [])
        centre = self.physical_centre()
        for hwnd in handles:
            if hwnd not in self.presented:
                ww.present(hwnd, centre=centre)
                self.presented.append(hwnd)
                log.info('Brought a Rekordbox window over Wave: %s', self.aside)

    def come_back(self):
        """Rekordbox is answered: Wave on top again, Rekordbox out of sight."""
        self.aside = None
        self.hide_banner('aside')
        if sys.platform == 'win32':
            try:
                from . import win_windows as ww
                for hwnd in self.presented:
                    ww.put_back(hwnd, minimize=self.session.view.get('hide_rekordbox', True))
            except Exception as exc:
                log.warning('Could not put Rekordbox back: %s', exc)
        self.presented = []
        self.set_on_top(bool(self.session.view.get('always_on_top', True)))
        self.raise_()
        self.activateWindow()
        log.info('Rekordbox answered: Wave is back in front')

    def set_on_top(self, on):
        """Wave above other windows, or not, without leaving full screen."""
        if sys.platform == 'win32':
            try:
                from . import win_windows as ww
                ww.user32.SetWindowPos(int(self.winId()), ww.HWND_TOPMOST if on else ww.HWND_NOTOPMOST, 0, 0, 0, 0,
                                       ww.SWP_NOMOVE | ww.SWP_NOSIZE | ww.SWP_NOACTIVATE)
            except Exception as exc:
                log.warning('Could not change Wave on top: %s', exc)
        elif self.windowHandle() is not None:
            self.windowHandle().setFlag(Qt.WindowType.WindowStaysOnTopHint, on)

    def physical_centre(self):
        """Wave's window centre in the physical pixels Win32 uses."""
        from .win_windows import window_rect
        left, top, right, bottom = window_rect(int(self.winId()))
        return (left + right) // 2, (top + bottom) // 2

    def page_rect(self):
        """The part of Wave below its toolbar, in physical pixels (Windows)."""
        from .win_windows import window_rect
        left, top, right, bottom = window_rect(int(self.winId()))
        offset = round(self.pages.mapTo(self, self.pages.rect().topLeft()).y() * self.devicePixelRatioF())
        return left, top + offset, right, bottom

    @staticmethod
    def rekordbox_window_alive(hwnd):
        try:
            from .win_windows import alive
            return alive(hwnd)
        except Exception:
            return False

    def show_banner(self, text, tone='info', action=None, timeout=None, key=None):
        """A one-line message under the toolbar; ``action`` is (label, callable)."""
        self.banner_key = key
        self.banner_text.setText(text)
        self.banner.setProperty('tone', tone)
        self.banner.style().unpolish(self.banner)
        self.banner.style().polish(self.banner)
        self.banner_callback = action[1] if action else None
        self.banner_action.setText(action[0] if action else '')
        self.banner_action.setVisible(bool(action))
        self.banner.show()
        if timeout:
            self.banner_timer.start(timeout)
        else:
            self.banner_timer.stop()

    def hide_banner(self, key=None):
        if key is None or key == self.banner_key:
            self.banner.hide()
            self.banner_key = None

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
                QMessageBox.information(self, 'Check for updates', f'Rekordbox Wave {result.version} is ready: press UPDATE at the top.'
                                        if result else f'Rekordbox Wave {__version__} is the latest version.')
        else:
            self.updating = True
            self.update_button.setText('RESTARTING…')
            self.close()
            QApplication.quit()

    def show_rekordbox(self):
        """Rekordbox itself, for its preferences and so on: Wave steps out of the way until it is used again."""
        from .mac_bridge import show
        try:
            self.set_on_top(False)
            show()
            if sys.platform == 'win32':
                self.showMinimized()
        except Exception as exc:
            self.set_on_top(bool(self.session.view.get('always_on_top', True)))
            QMessageBox.information(self, 'Rekordbox', str(exc))

    def changeEvent(self, event):
        # Back in Wave after Rekordbox was shown: on top again, unless Wave is stepping aside for a question.
        if event.type() == QEvent.Type.ActivationChange and self.isActiveWindow() and not getattr(self, 'aside', None) \
                and not getattr(self, 'rekordbox_browsing', None) and hasattr(self, 'session'):
            self.set_on_top(bool(self.session.view.get('always_on_top', True)))
        super().changeEvent(event)

    def connect_rekordbox(self):
        from .mac_bridge import rekordbox_pid, restart
        why = ('Restart Rekordbox to finish updating Wave? It still runs the bridge from the Wave you updated from.'
               if getattr(self.session.reader, 'stale', False) else 'Restart Rekordbox so Wave can connect to it?')
        if rekordbox_pid() and QMessageBox.question(
                self, 'Connect Rekordbox', f'{why}\n\n'
                'Playback stops while it restarts. Rekordbox asks first if a deck is playing.') != QMessageBox.StandardButton.Yes:
            return
        self.update_status('Restarting Rekordbox with Wave…' if rekordbox_pid() else 'Starting Rekordbox with Wave…')
        if self.session.host:
            self.session.host.connect_now()
            return
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

    def pill(self, text):
        """The toolbar's connection light and a few words; the whole story is in its tooltip and Settings."""
        session = self.session
        state = getattr(session.reader, 'state', None)
        state = state if isinstance(state, str) else None
        doing = session.host.doing if session.host else ''
        if session.demo:
            colour, words = '#8d96a0', 'DEMO'
        elif getattr(self, 'aside', None):
            colour, words = '#ff9f1a', 'REKORDBOX NEEDS YOU'
        elif doing:
            colour, words = '#ff9f1a', doing.upper() + '…'
        elif state == 'live':
            colour, words = '#2fd16b', 'REKORDBOX'
        elif state in ('starting', 'busy'):
            colour, words = '#ff9f1a', 'REKORDBOX STARTING' if state == 'starting' else 'REKORDBOX BUSY'
        elif state in ('unsupported', 'failed'):
            colour, words = '#ff4d4d', 'REKORDBOX · READ ONLY' if getattr(session.reader, 'connected', False) else 'UNSUPPORTED REKORDBOX'
        elif state in ('none', 'older'):
            colour, words = '#ff9f1a', 'CONNECTING'
        elif state == 'absent':
            colour, words = '#ff4d4d', 'REKORDBOX OFF'
        else:
            return text if len(text) < 70 else text[:67] + '…'
        return f'<span style="color:{colour}; font-size:20px;">●</span>&nbsp;{words}'

    def update_status(self, text):
        self.status.setText(self.pill(text))
        self.status.setToolTip(text)
        if hasattr(self, 'connection_detail'):
            self.connection_detail.setText(text)

    def apply_theme(self):
        size = round(22 * self.session.view.get('settings_scale', 1))
        check_icon = (Path(__file__).parent / "data/check.svg").as_posix()
        chevron = (Path(__file__).parent / "data/chevron.svg").as_posix()
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
            QWidget#browser QWidget#crumbs { background:transparent; }
            QWidget#browser QPushButton#crumb { background:transparent; border:0; border-radius:6px; color:#8fb8ff;
                                                font-size:19px; font-weight:600; padding:6px 8px; min-height:30px; }
            QWidget#browser QPushButton#crumb:pressed { background:#1d2a3b; }
            QWidget#browser QLabel#crumbArrow { color:#5b636d; font-size:22px; background:transparent; padding:0 2px; }
            QWidget#browser QLabel#crumbHere { color:#f2f4f6; font-size:19px; font-weight:700; background:transparent; padding:0 6px; }
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
            QLabel#connection { color:#d9dde2; font-size:13px; font-weight:600; letter-spacing:1px; }
            QFrame#banner { background:#14304f; border-bottom:1px solid #1d4a7a; }
            QFrame#banner[tone="warn"] { background:#4a3108; border-bottom:1px solid #8a5a0c; }
            QFrame#banner QLabel#bannerText { color:#f2f4f6; font-size:16px; font-weight:600; background:transparent; }
            QFrame#banner QPushButton { padding:4px 14px; min-height:24px; font-size:14px; }
            QFrame#banner QPushButton#bannerClose { background:transparent; border:0; color:#c9ced4; font-size:18px; padding:2px 8px; }
            QFrame#banner QPushButton#bannerAction { color:#000; background:#ff9f1a; border-color:#ff9f1a; }
            QLabel#master { color:#ffa21a; font-size:14px; font-weight:700; background:transparent; padding:0 8px; letter-spacing:1px; }
            QLabel#battery { color:#d9dde2; font-size:14px; font-weight:600; background:transparent; padding:0 6px; }
            QWidget#settings QScrollArea, QWidget#settingsBody { background:transparent; }
            QWidget#settings QFrame#card { background:#14161a; border:1px solid #1f2328; border-radius:16px; }
            QWidget#settings QFrame#row, QWidget#settings QWidget#buttons { background:transparent; }
            QWidget#settings QFrame#rule { background:#1f2328; }
            QWidget#settings QLabel { background:transparent; }
            QWidget#settings QLabel#rowTitle { color:#f2f4f6; font-weight:600; }
            QWidget#settings QLabel#cardTitle { color:#6f7882; font-weight:700; letter-spacing:2px; padding:4px 0 6px 0; }
            QWidget#settings QLabel#aboutTitle { color:#ffffff; font-weight:700; padding:8px 0; }
            QWidget#settings QPushButton#primary { color:#ffffff; background:#1a79eb; border-color:#1a79eb; }
            QWidget#settings QPushButton#primary:hover { background:#2b87f3; }
            QWidget#settings QComboBox { background:#1d2026; border:1px solid #30353d; border-radius:10px; padding:8px 44px 8px 16px; }
            QWidget#settings QComboBox:hover { border-color:#4a525c; }
            QWidget#settings QComboBox:on { border-color:#1a79eb; }
            QWidget#settings QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:center right; width:40px; border:0; }
            QWidget#settings QComboBox QLineEdit { background:transparent; border:0; padding:0; }
            QComboBox QAbstractItemView { background:#1d2026; border:1px solid #30353d; border-radius:10px; padding:6px; outline:0;
                                          color:#e8eaed; selection-background-color:#1a79eb; }
            QComboBox QAbstractItemView::item { min-height:46px; padding:0 14px; border-radius:8px; }
            QComboBox QAbstractItemView::item:hover { background:#262a31; }
            QComboBox QAbstractItemView::item:selected { background:#1a79eb; color:#ffffff; }
            QWidget#settings QDoubleSpinBox { background:#1d2026; border:1px solid #30353d; border-radius:10px; padding:8px 12px; }
            QWidget#settings QDoubleSpinBox::up-button, QWidget#settings QDoubleSpinBox::down-button { width:28px; border:0; }
            QWidget#settings QFrame#segment { background:#1d2026; border:1px solid #30353d; border-radius:12px; }
            QWidget#settings QFrame#segment QPushButton { padding:8px 16px; border-radius:9px; min-height:24px; }
            QProgressBar { background:#1d2026; border:0; border-radius:3px; }
            QProgressBar::chunk { background:#1a79eb; border-radius:3px; }
        ''' + f'''
            QComboBox::down-arrow {{ image:url("{chevron}"); width:18px; height:18px; }}
            QCheckBox::indicator:checked {{ image:url("{check_icon}"); }}
            QWidget#settings, QWidget#settings * {{ font-size:{size}px; }}
            QWidget#settings QLabel#rowHelp {{ font-size:{round(size * .72)}px; color:#8d96a0; }}
            QWidget#settings QLabel#cardTitle {{ font-size:{round(size * .6)}px; }}
            QWidget#settings QLabel#pageHeading {{ font-size:{round(size * 1.25)}px; font-weight:700; color:#ffffff; padding:4px 0 2px 2px; }}
            QWidget#settings QLabel#aboutTitle {{ font-size:{round(size * 1.3)}px; }}
            QWidget#settings QPushButton {{ min-height:30px; font-size:{round(size * .74)}px; }}
            QWidget#settings QFrame#segment QPushButton {{ font-size:{round(size * .74)}px; }}
            QWidget#settings QLabel#pageTitle {{ font-size:{round(size * 1.45)}px; font-weight:700; }}
        ''')

    def build_settings(self):
        from .qt_settings import build
        return build(self)

    def refresh_settings(self):
        """Settings show what is true now: some choices also change outside this page (a tap on a deck's time)."""
        for name, key in (('time_segment', 'time_mode'), ('browser_segment', 'browser')):
            control = getattr(self, name, None)
            if control is not None:
                control.set_value(self.session.view.get(key))
        if getattr(self, 'count_segment', None) is not None:
            self.count_segment.set_value(self.session.render.deck_count)
        self.refresh_rekordbox_summary()
        label = getattr(self, 'controller_status', None)
        if label is not None:
            label.setText(self.controller_summary())

    def controller_summary(self):
        """Whether Wave reads the controller itself, which VIEW, BACK and the knob's push need."""
        midi = self.session.midi
        names = {'browse': 'the knob', 'browse_press': 'its push', 'back': 'BACK', 'view': 'VIEW', 'load': 'LOAD'}
        if midi is None:
            return ('Wave can\u2019t read the controller on this computer (Windows 11 with Windows MIDI Services can '
                    'share it with Rekordbox), so VIEW and BACK can\u2019t reach Wave: tap BROWSE. In the open browser '
                    'the knob still moves the highlight, through Rekordbox.')
        if not midi.controllers:
            return f'No controller with a Rekordbox map is connected yet. {midi.status}'.strip()
        found = [names[k] for k in names if k in midi.browser_controls]
        missing = [names[k] for k in names if k not in midi.browser_controls]
        heard = 'and has heard from it' if self.controller_seen else 'and is waiting to hear from it'
        text = f'Wave reads {", ".join(midi.controllers)} itself {heard}: {", ".join(found) or "no browser controls"}.'
        if missing:
            text += f' Not in its map: {", ".join(missing)} (OPEN LOG in System lists what was found).'
        return text

    def open_settings_page(self, name):
        if self.pages.currentWidget() is not self.settings_page:
            self.toggle_settings()
        for row in range(self.settings_menu.count()):
            if self.settings_menu.item(row).text() == name:
                self.settings_menu.setCurrentRow(row)

    def first_run(self, install=False, setup=False):
        """Wave without the Rekordbox it supports can't do its job: say so, and offer to fix it."""
        if sys.platform not in ('win32', 'darwin') or self.session.view.get('source') != 'Native':
            return
        from .rekordbox_setup import SUPPORTED_TEXT, summary, supported_install
        try:
            if supported_install() is not None:
                return
        except Exception as exc:
            log.warning('Could not check Rekordbox: %s', exc)
            return
        log.info('Supported Rekordbox missing: %s', summary())
        if install and getattr(self, 'install_button', None) is not None:
            self.open_settings_page('REKORDBOX')
            self.install_rekordbox()
            return
        if setup:
            self.open_settings_page('REKORDBOX')
        self.show_banner(f'{summary()}', 'warn', action=(f'INSTALL REKORDBOX {SUPPORTED_TEXT}', lambda: (
            self.open_settings_page('REKORDBOX'), self.install_rekordbox())), key='rekordbox')

    def refresh_rekordbox_summary(self):
        label = getattr(self, 'rekordbox_summary', None)
        if label is None:
            return
        from .rekordbox_setup import SUPPORTED_TEXT, summary, supported_install
        try:
            label.setText(summary())
            have = supported_install() is not None
        except Exception as exc:
            label.setText(f'Rekordbox could not be checked: {exc}')
            have = False
        self.install_button.setText('REINSTALL' if have else f'INSTALL REKORDBOX {SUPPORTED_TEXT}')
        self.install_button.setObjectName('' if have else 'primary')
        self.install_button.style().unpolish(self.install_button)
        self.install_button.style().polish(self.install_button)

    def install_rekordbox(self, choose=False):
        """Get AlphaTheta's installer for the supported Rekordbox (or use one the DJ downloaded) and run it."""
        from . import rekordbox_setup as rs
        if getattr(self, 'installing', None) and not self.installing.done():
            return
        archive = None
        if choose:
            archive, _ = QFileDialog.getOpenFileName(self, f'Rekordbox {rs.SUPPORTED_TEXT} installer', '',
                                                     'Rekordbox installer (*.zip *.exe *.pkg)')
            if not archive:
                return
        elif any(self.session.deck_playing(i) for i in range(4)):
            self.show_banner('Stop every deck first: installing Rekordbox restarts it.', 'warn', timeout=6000)
            return
        self.install_progress.setRange(0, 0)
        self.install_progress.show()
        self.install_text = 'Preparing…'

        def progress(done, total):
            self.install_fraction = done / total if total else None

        def status(text):
            self.install_text = text
        self.install_fraction = None
        self.installing = self.updates.submit(rs.install_supported, progress, archive, status)
        watch = QTimer(self)

        def poll():
            self.connection_detail.setText(self.install_text)
            if self.install_fraction is not None:
                self.install_progress.setRange(0, 1000)
                self.install_progress.setValue(int(self.install_fraction * 1000))
            if not self.installing.done():
                return
            watch.stop()
            self.install_progress.hide()
            try:
                self.installing.result()
                self.show_banner(f'Rekordbox {rs.SUPPORTED_TEXT} is installed: Wave starts it now.', 'info', timeout=8000)
                if self.session.host:
                    self.session.host.connect_now()
            except Exception as exc:
                log.warning('Rekordbox install: %s', exc)
                self.show_banner(str(exc), 'warn', action=('DOWNLOAD PAGE', lambda: QDesktopServices.openUrl(
                    QUrl(rs.DOWNLOAD_PAGE))), key='install')
            self.refresh_rekordbox_summary()
        watch.timeout.connect(poll)
        watch.start(200)

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
            self.refresh_settings()
        else:
            self.canvas.setFocus()
            self.pace_frames()

    def browse(self, by_knob=False):
        """BROWSE on screen, or VIEW on the controller: the browser chosen in Settings opens, or closes."""
        if self.rekordbox_browsing:
            self.leave_rekordbox()
        elif self.session.view.get('browser') == 'rekordbox' and self.pages.currentWidget() is not self.browser_page:
            self.browse_in_rekordbox()
        else:
            self.toggle_browser(by_knob=by_knob)

    def deck_tracks(self):
        paths = getattr(self.session.reader, 'deck_paths', None)
        return dict(paths) if paths else {i: getattr(deck, 'song_name', '') for i, deck in self.session.decks.items()}

    def browse_in_rekordbox(self):
        """Rekordbox's browser in front until a track is loaded. macOS, through the bridge: its Split Screen
        browser window, over Wave. Windows: its browser window - or its main window - laid over Wave below
        Wave's toolbar. Without Rekordbox to show, Wave's own browser."""
        reader = self.session.reader
        if sys.platform == 'darwin' and getattr(reader, 'bridged', False) and hasattr(reader, 'show_browser'):
            centre = self.frameGeometry().center()
            shown, why = reader.show_browser(True, (centre.x(), centre.y()))
            if shown:
                self.rekordbox_browsing = dict(kind='mac', tracks=self.deck_tracks())
                self.sync_nav()
            else:
                self.update_status(why)
                self.toggle_browser()
            return
        if sys.platform == 'win32':
            try:
                if self.browse_in_rekordbox_window():
                    return
            except Exception as exc:
                log.warning("Could not show Rekordbox's browser: %s", exc)
        elif sys.platform != 'darwin':
            from .mac_bridge import show
            try:
                show(maximize=True)  # while Wave is still in front, so the system lets it take the focus
                self.rekordbox_browsing = dict(kind='whole', tracks=self.deck_tracks(), state=self.windowState())
                self.showMinimized()
                return
            except Exception:
                pass
        self.show_banner("Rekordbox's browser needs Rekordbox running with Wave: here is Wave's browser meanwhile",
                         'info', timeout=5000, key='browser')
        self.toggle_browser()

    def browse_in_rekordbox_window(self):
        """Windows: True once Rekordbox's browser is up over Wave."""
        from . import win_windows as ww
        pid = getattr(self.session.reader, 'pid', None)
        if not pid:
            from .mac_bridge import rekordbox_pid
            pid = rekordbox_pid()
        if not pid:
            return False
        main, panels, _ = ww.classify(ww.windows_of(pid))
        target = next((w for w in panels if w.resizable), None)
        if target is None and main and not self.split_asked:
            # Only the browser, as Split Screen shows it, when Rekordbox's View menu can be reached.
            self.split_asked = True
            if ww.split_screen_from_menu(main.hwnd):
                QTimer.singleShot(900, self.browse_in_rekordbox)
                return True
        target = target or main
        if target is None:
            return False
        ww.present(target.hwnd, rect=self.page_rect())
        self.rekordbox_browsing = dict(kind='window', tracks=self.deck_tracks(), hwnd=target.hwnd,
                                       main=target is main)
        self.sync_nav()
        log.info("Rekordbox's %s is up over Wave for browsing", 'window' if target is main else 'browser')
        return True

    def leave_rekordbox(self, state=None):
        """Put Rekordbox's browser away and come back to the waveforms."""
        browsing, self.rekordbox_browsing = self.rekordbox_browsing, None
        self.sync_nav()
        if not browsing:
            return
        if browsing['kind'] == 'mac':
            try:
                self.session.reader.show_browser(False)
            except Exception as exc:
                log.warning("Could not close Rekordbox's browser: %s", exc)
            self.activateWindow()
            return
        if browsing['kind'] == 'window':
            try:
                from .win_windows import put_back
                put_back(browsing['hwnd'], minimize=not browsing['main'] or self.session.view.get('hide_rekordbox', True))
            except Exception as exc:
                log.warning("Could not put Rekordbox's browser away: %s", exc)
            self.raise_()
            self.activateWindow()
            self.pace_frames()
            return
        from .mac_bridge import hide
        try:
            hide()
        except Exception:
            pass  # Wave on top covers it anyway
        self.setWindowState(browsing['state'] & ~Qt.WindowState.WindowMinimized)
        self.show()
        self.raise_()
        self.activateWindow()
        self.pace_frames()

    def toggle_browser(self, by_knob=False):
        opening = self.pages.currentWidget() is not self.browser_page
        self.pages.setCurrentWidget(self.browser_page if opening else self.canvas)
        self.sync_nav()
        if opening and by_knob:
            # Opened from the controller: focus stays off the search box, so no on-screen keyboard pops up.
            page = self.browser_page
            (page.list if page.stack.currentWidget() is page.list else page.table).setFocus()
        elif opening:
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
        self.mark(self.browse_button, current is self.browser_page or bool(getattr(self, 'rekordbox_browsing', None)))
        self.mark(self.settings_button, current is self.settings_page)

    def set_count(self, count):
        self.session.render.deck_count = count
        if getattr(self, 'count_segment', None) is not None:
            self.count_segment.set_value(count)
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

    def on_knob_zoom(self, value):
        if not self.session.view.get('sampler_zoom', True):
            return
        self.session.knob_zoom(value, reverse=self.session.view.get('sampler_zoom_reverse', False))
        self.canvas.update()

    def zoom_by(self, factor):
        """Smooth zoom from a trackpad, wheel, controller or key: 4 to 512 beats across the lane."""
        beats = max(4.0, min(512.0, self.session.zoom_beats() * factor))
        self.session.view['zoom_beats'] = beats
        self.session.knob_beats = None  # a pinch or the keys take over from the knob until it moves again
        nearest = min(range(self.zoom_combo.count()), key=lambda i: abs(math.log2(self.zoom_combo.itemData(i) / beats)))
        self.zoom_combo.blockSignals(True)
        self.zoom_combo.setCurrentIndex(nearest)
        self.zoom_combo.blockSignals(False)
        self.canvas.update()
        self.save_zoom.start()

    def set_view(self, key, value):
        self.session.view[key] = value
        if key == 'zoom_beats':
            self.session.knob_beats = None  # a zoom picked in Settings takes over from the sampler knob
        self.session.configure_host()
        if key == 'keep_awake':
            self.awake.set(bool(value))
        if key == 'tap_to_open':
            self.browser_page.open_on = value
        if key == 'touch_keyboard_off':
            self.touch.start() if value else self.touch.stop()
        if key == 'dim':
            self.dimmer.refresh()
        self.canvas.update()
        self.persist()

    def update_toolbar_readouts(self):
        """Once a second: the tempo master's BPM, and the battery, with a warning when the charger comes out."""
        view = self.session.view
        master = ''
        if view.get('master_bpm', True):
            for idx, status in getattr(self.session.reader, 'deck_status', {}).items():
                if getattr(status, 'master', False) and getattr(status, 'bpm', 0):
                    master = f'MASTER {idx + 1} · {status.bpm:.1f}'
        self.master_label.setText(master)
        self.master_label.setVisible(bool(master))
        battery = None
        if view.get('battery', True):
            try:
                import psutil
                battery = psutil.sensors_battery()
            except Exception:
                battery = None
        self.battery_label.setVisible(battery is not None)
        if battery is None:
            return
        percent, plugged = round(battery.percent), bool(battery.power_plugged)
        colour = '#2fd16b' if plugged else '#ff4d4d' if percent <= 20 else '#ff9f1a' if percent <= 40 else '#d9dde2'
        self.battery_label.setText(f'<span style="color:{colour}">{"⚡" if plugged else "▮"} {percent}%</span>')
        if self.battery_plugged is True and not plugged:
            self.show_banner(f'Running on battery: {percent}%. Check the charger.', 'warn', timeout=15000, key='battery')
            log.warning('Charger unplugged at %d%%', percent)
        elif plugged and self.banner_key == 'battery':
            self.hide_banner('battery')
        if not plugged and percent <= 15 and self.banner_key != 'battery':
            self.show_banner(f'Battery low: {percent}%. Plug in the charger.', 'warn', key='battery')
        self.battery_plugged = plugged

    def open_log(self):
        folder = config_directory() / 'logs'
        folder.mkdir(parents=True, exist_ok=True)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(folder)))

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
        log.info('Wave is closing')
        if self.rekordbox_browsing:
            self.leave_rekordbox()
        host = self.session.host
        if host and self.session.view.get('quit_rekordbox', True) and not self.updating:
            host.quit(playing=any(self.session.deck_playing(i) for i in range(4)))
        self.awake.set(False)
        self.touch.stop()  # Windows' touch keyboard as it was
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
    parser.add_argument('--setup', action='store_true', help='Open Rekordbox setup if the supported Rekordbox is missing')
    parser.add_argument('--install-rekordbox', action='store_true', help='Install the supported Rekordbox now if it is missing')
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setApplicationName('Rekordbox Wave')
    app.setOrganizationName('Rekordbox Wave')
    app.setStyle('Fusion')
    from PySide6.QtGui import QIcon
    app.setWindowIcon(QIcon(str(Path(__file__).parent / 'data' / 'icon.png')))
    from .gig import SingleInstance, setup_logging
    logged = setup_logging(config_directory() / 'logs')
    log.info('Rekordbox Wave %s starting on %s (%s)', __version__, sys.platform, 'demo' if args.demo else 'live')
    instance = None
    if not args.smoke_test and not args.demo:
        instance = SingleInstance()
        if not instance.primary:
            log.info('Wave is already running: brought it forward')
            return 0
    from .updater import clean_up
    try:
        clean_up()
    except OSError:
        pass  # a leftover of an earlier update; it is tried again next start
    session = Session(demo=args.demo, config_path=args.config)
    window = MainWindow(session)
    window.show()
    if instance:
        def forward():
            window.showNormal() if window.isMinimized() else None
            window.raise_()
            window.activateWindow()
        instance.listen(forward)
    if not logged:
        log.warning('No log file: the settings folder is not writable')
    if not args.demo and not args.smoke_test:
        QTimer.singleShot(1200, lambda: window.first_run(install=args.install_rekordbox, setup=args.setup))
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
