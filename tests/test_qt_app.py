"""Native acceptance checks for the single-window UI and physical full-screen bounds."""
import ctypes
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from PySide6.QtCore import QPoint, QRectF, Qt
from PySide6.QtWidgets import QApplication, QCheckBox, QPushButton
from PySide6.QtTest import QTest
from rb_waveform_core.qt_app import Session, MainWindow
from rb_waveform_core.qt_display import screen_key, flag_positions


class QtAppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setStyle('Fusion')
        cls.app.setQuitOnLastWindowClosed(False)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.session = Session(demo=True, config_path=Path(self.temp.name)/'settings.json')
        self.window = MainWindow(self.session)
        self.window.show()
        QTest.qWait(60)

    def tearDown(self):
        self.window.close()
        self.app.processEvents()
        self.temp.cleanup()

    def test_settings_are_inside_the_same_window_and_text_is_readable(self):
        self.window.toggle_settings()
        self.assertEqual(self.window.pages.currentIndex(), 1)
        self.assertIs(self.window.settings_page.window(), self.window)
        self.assertIn('font-size:22px', self.window.styleSheet())
        self.window.set_settings_scale(1.5)
        self.assertIn('font-size:33px', self.window.styleSheet())
        self.window.escape()
        self.assertEqual(self.window.pages.currentIndex(), 0)

    def test_connection_failure_is_visible_in_empty_lanes(self):
        self.session.demo = False
        self.session.decks[0].analysis = None
        message = 'No Mac memory layout for Rekordbox 7.2.19.0342'
        self.session.set_status(message)
        painter = Mock()
        self.window.canvas.draw_deck(painter, 0, QRectF(0, 0, 1280, 180))
        self.assertEqual(painter.drawText.call_args.args[-1], message)

    def test_browser_closes_once_a_track_lands_on_a_deck(self):
        reader = Mock(bridged=True, deck_paths={0: '/a.mp3', 1: ''})
        self.session.reader = reader
        self.window.sync_load_targets()
        self.window.toggle_browser()
        self.window.sync_load_targets()
        reader.deck_paths = {0: '/a.mp3', 1: ''}  # unchanged: stays open
        self.window.sync_load_targets()
        self.assertIs(self.window.pages.currentWidget(), self.window.browser_page)
        reader.deck_paths = {0: '/a.mp3', 1: '/b.mp3'}  # loaded on deck 2, from Wave or the controller
        self.window.sync_load_targets()
        self.assertIs(self.window.pages.currentWidget(), self.window.canvas)

    def test_windows_rekordbox_browser_is_laid_over_wave_and_put_away_after_a_load(self):
        from rb_waveform_core.win_windows import Win
        main = Win(11, 'rekordbox', False, True, (0, 0, 1600, 900), False, True, False)
        reader = Mock(bridged=True, pid=42, deck_paths={0: '/a.mp3', 1: ''})
        self.session.reader = reader
        self.session.view['browser'] = 'rekordbox'
        with patch('rb_waveform_core.win_windows.windows_of', return_value=[main]), \
                patch('rb_waveform_core.win_windows.split_screen_from_menu', return_value=False), \
                patch('rb_waveform_core.win_windows.window_rect', return_value=(0, 0, 1920, 1080)), \
                patch('rb_waveform_core.win_windows.alive', return_value=True), \
                patch('rb_waveform_core.win_windows.present') as present, \
                patch('rb_waveform_core.win_windows.put_back') as put_back, \
                patch.object(self.window, 'pace_frames'), \
                patch('rb_waveform_core.qt_app.sys.platform', 'win32'):
            self.window.browse()
            hwnd, = present.call_args.args
            left, top, right, bottom = present.call_args.kwargs['rect']
            self.assertEqual((hwnd, left, right, bottom), (11, 0, 1920, 1080))
            self.assertGreater(top, 0)  # below Wave's toolbar, which stays usable
            self.assertFalse(self.window.isMinimized())
            self.assertTrue(self.window.browse_button.property('selected'))
            self.window.sync_load_targets()  # nothing loaded yet: Rekordbox stays up
            put_back.assert_not_called()
            reader.deck_paths = {0: '/a.mp3', 1: '/b.mp3'}  # picked in Rekordbox, loaded on deck 2
            self.window.sync_load_targets()
            put_back.assert_called_once_with(11, minimize=True)
            self.assertIsNone(self.window.rekordbox_browsing)
            self.window.browse()  # BROWSE again opens it again; and again puts it away
            self.window.browse()
            self.assertEqual(put_back.call_count, 2)

    def test_rekordbox_browser_falls_back_to_waves_without_rekordbox(self):
        self.session.reader = Mock(bridged=False, pid=None, deck_paths={})
        with patch('rb_waveform_core.mac_bridge.rekordbox_pid', return_value=None), \
                patch('rb_waveform_core.qt_app.sys.platform', 'win32'):
            self.window.browse()
        self.assertIs(self.window.pages.currentWidget(), self.window.browser_page)

    def test_mac_rekordbox_browser_opens_its_split_screen_window_over_wave(self):
        reader = Mock(bridged=True, deck_paths={0: '/a.mp3', 1: ''})
        reader.show_browser.return_value = (True, "Opening Rekordbox's browser")
        self.session.reader = reader
        self.session.view['browser'] = 'rekordbox'
        with patch('rb_waveform_core.qt_app.sys.platform', 'darwin'):
            self.window.browse()
            centre = self.window.frameGeometry().center()
            reader.show_browser.assert_called_once_with(True, (centre.x(), centre.y()))
            self.assertFalse(self.window.isMinimized())  # Wave stays under Rekordbox's browser window
            self.window.sync_load_targets()
            self.assertIsNotNone(self.window.rekordbox_browsing)  # still browsing: nothing loaded
            reader.deck_paths = {0: '/a.mp3', 1: '/b.mp3'}
            self.window.sync_load_targets()
            reader.show_browser.assert_called_with(False)
            self.assertIsNone(self.window.rekordbox_browsing)

    def test_the_knob_moves_waves_own_browser_and_never_jumps_to_rekordboxs_list(self):
        from rb_waveform_core.mac_bridge import Browser
        from rb_waveform_core.catalog import CatalogTrack
        state = dict(rows=500, cursor=20, first=0, focus='list', tree_path='', tracks=((1, '/a'),))
        reader = Mock(bridged=True, devices=(), browser=Browser(**state))
        self.session.reader = reader
        self.assertEqual(self.session.view['browser'], 'wave')
        page = self.window.browser_page
        self.window.follow_rekordbox_browser()  # first look: no move
        self.assertIs(self.window.pages.currentWidget(), self.window.canvas)
        reader.browser = Browser(**{**state, 'cursor': 21})
        self.window.follow_rekordbox_browser()  # the first turn opens Wave's browser where it was left
        self.assertIs(self.window.pages.currentWidget(), page)
        rows = [CatalogTrack(str(i), f'Track {i}', 'Artist', 120.0, '', 200.0, f'/music/{i}.mp3') for i in range(30)]
        page.stack.setCurrentWidget(page.table)
        page.model.replace(rows, len(rows))
        page.table.selectRow(5)
        with patch.object(page, 'mirror') as mirror:
            reader.browser = Browser(**{**state, 'cursor': 24})  # three clicks clockwise
            self.window.follow_rekordbox_browser()
            reader.browser = Browser(**{**state, 'cursor': 23})  # one back
            self.window.follow_rekordbox_browser()
            reader.browser = Browser(**{**state, 'cursor': 23, 'rows': 501})  # Rekordbox refreshed: not a turn
            self.window.follow_rekordbox_browser()
        mirror.assert_not_called()
        self.assertEqual(page.selected_track().title, 'Track 7')
        self.assertNotEqual(page.view, ('rekordbox',))

    def test_controller_load_loads_the_track_picked_in_waves_browser(self):
        from rb_waveform_core.mac_bridge import Browser
        from rb_waveform_core.catalog import CatalogTrack
        reader = Mock(bridged=True, devices=(), deck_paths={0: '/music/old.mp3', 1: ''},
                      browser=Browser(rows=9, cursor=0, first=0, focus='list', tree_path='', tracks=((7, '/music/rb.mp3'),)))
        self.session.reader = reader
        self.window.follow_rekordbox_browser()
        self.window.sync_load_targets()
        self.window.toggle_browser()
        page = self.window.browser_page
        page.stack.setCurrentWidget(page.table)
        page.model.replace([CatalogTrack('12', 'Mine', 'Me', 124.0, '', 300.0, '/music/mine.mp3')], 1)
        page.table.selectRow(0)
        with patch.object(self.session, 'load_track', return_value=True) as load:
            reader.deck_paths = {0: '/music/old.mp3', 1: '/music/rb.mp3'}  # LOAD on the controller, deck 2
            self.session.loading = ('busy',)
            self.window.sync_load_targets()
            load.assert_called_once()
            self.assertEqual((load.call_args.args[0], load.call_args.args[1].title), (1, 'Mine'))
            self.assertIs(self.window.pages.currentWidget(), page)  # waits for Wave's pick to land
            reader.deck_paths = {0: '/music/old.mp3', 1: '/music/mine.mp3'}
            self.session.loading = None
            self.window.sync_load_targets()
        self.assertIs(self.window.pages.currentWidget(), self.window.canvas)

    def test_the_controllers_knob_push_opens_the_browser_and_back_closes_it(self):
        page = self.window.browser_page
        self.session.controllerBrowse.emit('browse_press', 0, -1)  # pushed on the waveforms
        self.assertIs(self.window.pages.currentWidget(), page)
        from rb_waveform_core.catalog import Category
        page.navigate(('list', 'PLAYLIST', None), 'PLAYLIST')
        page.list_ready(page.view, [Category('a', 'Warm up', 3), Category('b', 'Peak', 5)])
        self.session.controllerBrowse.emit('browse', 2, -1)
        self.assertEqual(page.list.currentIndex().row(), 1)
        with patch.object(page, 'request'):
            self.session.controllerBrowse.emit('browse_press', 0, -1)  # into the highlighted playlist
        self.assertEqual(page.view, ('tracks', ('PLAYLIST', 'b')))
        self.session.controllerBrowse.emit('back', 0, -1)
        self.assertEqual(page.view[0], 'list')
        self.session.controllerBrowse.emit('back', 0, -1)  # BACK at the top: the waveforms again
        self.assertIs(self.window.pages.currentWidget(), self.window.canvas)

    def test_without_the_controller_a_change_in_rekordboxs_list_opens_the_browser(self):
        from rb_waveform_core.mac_bridge import Browser
        state = dict(rows=10, cursor=0, first=0, focus='', tree_path='', tracks=((1, '/a'),))
        reader = Mock(bridged=True, devices=(), browser=Browser(**state))
        self.session.reader = reader
        self.session.midi = None
        self.window.follow_rekordbox_browser()
        with patch.object(self.window.browser_page, 'run') as run:
            reader.browser = Browser(**{**state, 'rows': 4, 'tracks': ((9, '/b'),)})  # the knob pressed in its tree
            self.window.follow_rekordbox_browser()
        self.assertIs(self.window.pages.currentWidget(), self.window.browser_page)
        run.assert_called_once()  # and Wave looks up which playlist that is, to show the same one

    def test_a_stray_controller_load_with_nothing_picked_puts_the_deck_back(self):
        from rb_waveform_core.catalog import CatalogTrack
        reader = Mock(bridged=True, devices=(), browser=None, deck_paths={0: '/music/mine.mp3'})
        self.session.reader = reader
        self.window.sync_load_targets()
        self.window.toggle_browser()
        page = self.window.browser_page
        page.model.replace([], 0)
        old = CatalogTrack('42', 'Mine', 'Me', 124.0, '', 300.0, '/music/mine.mp3')
        with patch.object(page, 'run', lambda f, callback, *a: callback({'/music/mine.mp3': old})), \
                patch.object(self.session, 'load_track', return_value=True) as load:
            reader.deck_paths = {0: '/rekordbox/Sampler/Crash.wav'}  # LOAD on the controller
            self.window.sync_load_targets()
        load.assert_called_once()
        self.assertEqual((load.call_args.args[0], load.call_args.args[1].identifier), (0, '42'))
        self.assertIs(self.window.pages.currentWidget(), page)

    def test_waves_own_loads_are_never_mistaken_for_the_controllers(self):
        self.session.wave_loads[1] = ('/music/b.mp3', time.monotonic() + 15)
        self.assertTrue(self.session.loaded_by_wave(1, '/music/b.mp3'))
        self.assertFalse(self.session.loaded_by_wave(1, '/music/crash.wav'))
        self.assertFalse(self.session.loaded_by_wave(0, '/music/b.mp3'))

    def test_breadcrumbs_go_back_to_any_level(self):
        page = self.window.browser_page
        with patch.object(page, 'run'):
            page.open_category('PLAYLIST')
            page.trail = [('PLAYLIST', ('list', 'PLAYLIST', None)), ('Gigs', ('list', 'PLAYLIST', 'f'))]
            page.navigate(('tracks', ('PLAYLIST', 'p')), 'Friday')
            crumbs = [w.text() for w in page.crumbs.findChildren(QPushButton)]
            self.assertEqual(crumbs, ['COLLECTION', 'PLAYLIST', 'Gigs'])
            page.crumbs.findChildren(QPushButton)[2].click()  # the folder just left
            self.assertEqual((page.view, page.trail), (('list', 'PLAYLIST', 'f'), [('PLAYLIST', ('list', 'PLAYLIST', None))]))
            page.crumbs.findChildren(QPushButton)[0].click()  # the source
            self.assertEqual(page.view, ('sources',))

    def test_a_new_category_starts_without_the_last_search(self):
        page = self.window.browser_page
        page.search.setText('nine')
        self.assertEqual(page.view, ('tracks', None))
        page.open_category('PLAYLIST')
        self.assertEqual(page.search.text(), '')

    def test_touching_the_overview_previews_that_part_and_glides_back(self):
        self.session.preview_hold(0, 120.0)
        self.assertEqual(self.session.preview_position(0), 120.0)
        self.session.preview_release(0)
        now = time.monotonic()
        self.assertEqual(self.session.preview_position(0, now + 1), 120.0)  # held a moment after lifting
        between = self.session.preview_position(0, now + 3.2)
        self.assertIsNotNone(between)
        self.assertIsNone(self.session.preview_position(0, now + 4))  # back on the playhead
        self.assertIsNone(self.session.preview_position(1))

    def test_tapping_the_time_flips_remaining_and_elapsed(self):
        mode = self.session.view['time_mode']
        self.window.canvas.timeModeToggled.emit()
        self.assertNotEqual(self.session.view['time_mode'], mode)

    def test_the_version_is_in_the_title(self):
        from rb_waveform_core import __version__
        self.assertEqual(self.window.windowTitle(), f'Rekordbox Wave {__version__}')

    def test_check_for_updates_answers_even_when_up_to_date(self):
        with patch('rb_waveform_core.updater.check', return_value=None), \
                patch('rb_waveform_core.qt_app.QMessageBox.information') as told:
            self.window.check_for_update(asked=True)  # demo sessions skip only the automatic check
            self.window.update_job[1].result()
            self.window.poll_update()
        self.assertIn('is the latest version', told.call_args.args[2])
        self.assertFalse(self.window.update_button.isVisible())

    def test_rekordbox_stuck_starting_is_brought_forward_once_then_wave_returns(self):
        self.session.reader = Mock(status='Rekordbox is starting')
        with patch('rb_waveform_core.mac_bridge.show') as show, patch.object(self.window, 'activateWindow') as back, \
                patch('rb_waveform_core.qt_app.sys.platform', 'darwin'):
            self.window.watch_rekordbox_start(100)
            self.window.watch_rekordbox_start(105)
            show.assert_not_called()  # an ordinary start
            self.window.watch_rekordbox_start(115)
            show.assert_not_called()  # a cold start takes a while
            self.window.watch_rekordbox_start(121)
            show.assert_called_once()  # waiting on its own questions
            self.assertIn('answer its questions', self.window.status.toolTip())
            self.session.reader.status = 'REKORDBOX · direct'
            self.session.reader.state = 'live'
            self.window.watch_rekordbox_start(122)
            back.assert_called_once()
            self.session.reader.state = 'busy'
            self.session.reader.status = 'REKORDBOX · busy'  # stuck later: a question on load, a drive that hangs
            self.window.watch_rekordbox_start(200)
            self.window.watch_rekordbox_start(209)
            self.assertEqual(show.call_count, 2)
            self.assertIn('check your music drive', self.window.status.toolTip())

    def test_a_rekordbox_popup_wave_could_not_close_is_put_in_front_until_answered(self):
        from rb_waveform_core.popups import Popup
        watcher = Mock(popups=[], closed=[], error='')
        self.session.popups = watcher
        self.session.reader = Mock(state='live', status='REKORDBOX · direct', pid=42)
        with patch('rb_waveform_core.mac_bridge.show') as show, patch('rb_waveform_core.qt_app.sys.platform', 'darwin'), \
                patch.object(self.window, 'set_on_top') as on_top:
            watcher.popups = [Popup('w', 'Library', ('Sync your library now?',), ('Sync', 'Cancel'), first_seen=100)]
            self.window.watch_rekordbox(100.5)
            show.assert_not_called()  # a moment's grace, in case Wave closes it
            self.window.watch_rekordbox(102)
            show.assert_called_once()
            on_top.assert_called_with(False)
            self.assertTrue(self.window.banner.isVisibleTo(self.window))
            self.assertIn('Sync your library now?', self.window.banner_text.text())
            self.window.watch_rekordbox(103)
            show.assert_called_once()  # once, not every check
            watcher.popups = []  # answered
            self.window.watch_rekordbox(104)
            on_top.assert_called_with(True)
            self.assertIsNone(self.window.aside)
            self.assertFalse(self.window.banner.isVisibleTo(self.window))

    def test_notices_wave_closed_are_told_in_the_banner(self):
        self.session.popups = Mock(popups=[], closed=[(50.0, "What's new in rekordbox")], error='')
        self.window.watch_rekordbox(51)
        self.assertIn("Closed a Rekordbox notice: What's new", self.window.banner_text.text())

    def test_the_status_light_says_how_rekordbox_is(self):
        self.session.demo = False
        self.session.reader = Mock(state='live', connected=True)
        self.assertIn('REKORDBOX', self.window.pill('REKORDBOX · direct'))
        self.assertIn('#2fd16b', self.window.pill(''))
        self.session.reader.state = 'absent'
        self.assertIn('REKORDBOX OFF', self.window.pill(''))
        self.session.host = Mock(doing='Starting Rekordbox')
        self.assertIn('STARTING REKORDBOX…', self.window.pill(''))

    def test_settings_from_before_2_5_move_to_rekordboxs_browser(self):
        path = Path(self.temp.name) / 'old.json'
        path.write_text(json.dumps({'view': {'browser': 'wave'}}))
        self.session.load_config(path)
        self.assertEqual(self.session.view['browser'], 'rekordbox')
        path.write_text(json.dumps({'view': {'browser': 'wave', 'settings_version': 2}}))
        self.session.load_config(path)
        self.assertEqual(self.session.view['browser'], 'wave')  # chosen since: kept

    def test_two_and_four_decks_reuse_the_same_window(self):
        handle = self.window.winId()
        self.window.set_count(2)
        self.assertEqual(self.session.order(), (0, 1))
        self.window.set_count(4)
        self.assertEqual(self.session.order(), (2, 0, 1, 3))
        self.assertEqual(handle, self.window.winId())

    def test_selecting_monitor_automatically_fills_entire_panel(self):
        self.native_fullscreen()
        screen = self.app.screens()[0]
        self.window.select_screen(1)
        self.settle(lambda: self.window.isFullScreen() and self.window.geometry() == screen.geometry())
        self.assertTrue(self.session.render.fill_display)
        self.assertTrue(self.window.isFullScreen())
        self.assertEqual(self.window.geometry(), screen.geometry())
        self.assertEqual(self.session.render.display_key, screen_key(screen))
        if sys.platform == 'win32':
            self.assert_native_windows_coverage()
        self.window.toggle_settings()
        QTest.qWait(50)
        self.assertTrue(self.window.isFullScreen())
        self.assertEqual(self.window.geometry(), screen.geometry())
        self.window.escape()  # returns to waves while retaining full screen
        self.assertTrue(self.window.isFullScreen())
        self.window.escape()
        self.settle(lambda: not self.window.isFullScreen())
        self.assertFalse(self.window.isFullScreen())
        self.window.ensure_display()
        self.assertFalse(self.window.isFullScreen())

    def test_waveform_style_toggle_and_classic_colours_move_to_rekordbox(self):
        from rb_waveform_core.qt_app import STYLES
        self.assertEqual(self.session.colors.low_color, (0, 84, 226))
        self.window.set_style('Wave classic')
        self.assertEqual(self.session.colors.high_color, (255, 255, 255))
        saved = json.loads((Path(self.temp.name) / 'settings.json').read_text())
        self.assertEqual(saved['view']['style'], 'Wave classic')
        # An older profile (no style saved) with the untouched classic colours gets the new look.
        del saved['view']['style']
        old = Path(self.temp.name) / 'old.json'
        old.write_text(json.dumps(saved))
        self.session.load_config(old)
        self.assertEqual(tuple(self.session.colors.low_color), STYLES['Rekordbox']['low_color'])
        # 2.2.0's first sampled palette also moves to the measured one; custom colours stay.
        from rb_waveform_core.qt_app import FIRST_REKORDBOX
        saved['view']['style'] = 'Rekordbox'
        saved['color'].update({k: list(v) for k, v in FIRST_REKORDBOX.items()})
        old.write_text(json.dumps(saved))
        self.session.load_config(old)
        self.assertEqual(tuple(self.session.colors.mid_color), (255, 166, 0))
        saved['color']['low_color'] = [10, 20, 30]
        old.write_text(json.dumps(saved))
        self.session.load_config(old)
        self.assertEqual(tuple(self.session.colors.low_color), (10, 20, 30))

    def test_trackpad_and_wheel_zoom_smoothly_within_limits(self):
        from PySide6.QtCore import QPoint, QPointF
        from PySide6.QtGui import QWheelEvent
        self.session.view['zoom_beats'] = 32
        def scroll(dy):
            event = QWheelEvent(QPointF(100, 100), QPointF(100, 100), QPoint(0, 0), QPoint(0, dy),
                                Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier, Qt.ScrollPhase.NoScrollPhase, False)
            self.app.sendEvent(self.window.canvas, event)
        scroll(120)  # one notch up: zoom in a quarter of a doubling
        self.assertAlmostEqual(self.session.view['zoom_beats'], 32 * 2 ** -.25)
        scroll(-240)
        self.assertAlmostEqual(self.session.view['zoom_beats'], 32 * 2 ** .25)
        for _ in range(100):
            scroll(-480)
        self.assertEqual(self.session.view['zoom_beats'], 512)
        self.assertEqual(self.window.zoom_combo.currentData(), 512)
        self.window.zoom_by(1e-6)
        self.assertEqual(self.session.view['zoom_beats'], 4)

    def test_remembered_monitor_is_filled_whenever_it_is_connected(self):
        self.native_fullscreen()
        screen = self.app.screens()[0]
        self.session.render.fill_display = True
        self.session.render.display_key = 'Portable|15 inch|unplugged'
        self.window.ensure_display()  # not connected: stay a window
        self.assertFalse(self.window.isFullScreen())
        self.session.render.display_key = screen_key(screen)
        self.window.screens_changed(plugged=True)  # plugged in: snap to it
        self.settle(lambda: self.window.isFullScreen() and self.window.geometry() == screen.geometry())
        self.assertTrue(self.window.isFullScreen())
        self.assertEqual(self.window.geometry(), screen.geometry())
        self.window.escape()  # windowed for now, but still remembered
        self.settle(lambda: not self.window.isFullScreen())
        self.window.ensure_display()
        self.assertFalse(self.window.isFullScreen())
        self.assertTrue(self.session.render.fill_display)
        self.window.screens_changed(plugged=True)
        self.settle(lambda: self.window.isFullScreen() and self.window.geometry() == screen.geometry())
        self.assertTrue(self.window.isFullScreen())

    def settle(self, done, timeout=3.0):
        # Native full-screen transitions animate.
        deadline = time.monotonic() + timeout
        while not done() and time.monotonic() < deadline:
            QTest.qWait(50)
        QTest.qWait(50)

    def native_fullscreen(self):
        # macOS completes full screen only for a foreground app bundle, never for a test
        # process; the same logic runs offscreen and on Windows' native high-DPI display.
        if self.app.platformName() == 'cocoa':
            self.skipTest('native macOS full screen needs a foreground app bundle')

    def assert_native_windows_coverage(self):
        from ctypes import wintypes
        class MonitorInfo(ctypes.Structure):
            _fields_ = [('size',wintypes.DWORD),('monitor',wintypes.RECT),('work',wintypes.RECT),('flags',wintypes.DWORD)]
        u = ctypes.windll.user32
        u.MonitorFromWindow.argtypes = [wintypes.HWND,wintypes.DWORD]
        u.MonitorFromWindow.restype = wintypes.HANDLE
        u.GetMonitorInfoW.argtypes = [wintypes.HANDLE,ctypes.POINTER(MonitorInfo)]
        u.GetWindowRect.argtypes = [wintypes.HWND,ctypes.POINTER(wintypes.RECT)]
        u.WindowFromPoint.argtypes = [wintypes.POINT]
        u.WindowFromPoint.restype = wintypes.HWND
        u.GetWindowThreadProcessId.argtypes = [wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
        hwnd = int(self.window.winId())
        info = MonitorInfo(); info.size = ctypes.sizeof(info)
        self.assertTrue(u.GetMonitorInfoW(u.MonitorFromWindow(hwnd,2),ctypes.byref(info)))
        actual = wintypes.RECT()
        self.assertTrue(u.GetWindowRect(hwnd,ctypes.byref(actual)))
        expected = info.monitor
        self.assertEqual((actual.left,actual.top,actual.right,actual.bottom),
                         (expected.left,expected.top,expected.right,expected.bottom))
        # In particular, the bottom corners belong to our app, not Explorer's taskbar.
        for x,y in [(expected.left+2,expected.top+2),(expected.right-3,expected.bottom-3),
                    (expected.left+2,expected.bottom-3)]:
            owner = wintypes.DWORD()
            u.GetWindowThreadProcessId(u.WindowFromPoint(wintypes.POINT(x,y)),ctypes.byref(owner))
            self.assertEqual(owner.value, os.getpid(), 'Desktop or taskbar is exposed at monitor edge')

    def test_profiles_restore_layout_and_scale(self):
        self.window.set_count(2)
        self.window.set_view('settings_scale', 1.4)
        self.window.set_render('deck_order', '4,2,1,3')
        self.session.save()
        data=json.loads(self.session.path.read_text())
        self.assertEqual(data['view']['settings_scale'],1.4)
        self.assertEqual(data['render']['deck_order'],'4,2,1,3')
        self.session.load_config(self.session.path)
        self.assertEqual(self.session.order(),(1,0))

    def test_invalid_profile_does_not_replace_valid_state(self):
        before=self.session.view.copy()
        bad=Path(self.temp.name)/'bad.json'
        bad.write_text(json.dumps({'view':{'playhead':'nan'}}))
        with self.assertRaises(ValueError):
            self.session.load_config(bad)
        self.assertEqual(self.session.view,before)

    def test_coincident_cue_flags_are_distinct(self):
        places=flag_positions([100]*8,24,600)
        self.assertEqual(len(places),len(set(places)))
        self.assertNotIn(None,places)

    def test_manual_analysis_keeps_native_playback_connection(self):
        self.session.demo = False
        with patch('rb_waveform_core.qt_app.QFileDialog.getOpenFileName', return_value=('/music/ANLZ.DAT','')), \
             patch.object(self.session,'open_track') as load:
            self.window.open_analysis()
        self.assertEqual(self.session.view['source'],'Native')
        load.assert_called_once_with(0,'/music/ANLZ.DAT')


if __name__=='__main__':
    unittest.main()
