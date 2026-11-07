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
from PySide6.QtWidgets import QApplication, QCheckBox
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

    def test_rekordbox_browser_setting_opens_rekordbox_and_returns_after_a_load(self):
        reader = Mock(bridged=True, deck_paths={0: '/a.mp3', 1: ''})
        self.session.reader = reader
        self.session.view['browser'] = 'rekordbox'
        with patch('rb_waveform_core.mac_bridge.show') as show, patch('rb_waveform_core.mac_bridge.hide') as hide:
            self.window.browse()
            show.assert_called_once_with(maximize=True)
            self.assertTrue(self.window.isMinimized())
            self.assertIsNot(self.window.pages.currentWidget(), self.window.browser_page)
            self.window.sync_load_targets()  # nothing loaded yet: Rekordbox stays up
            hide.assert_not_called()
            reader.deck_paths = {0: '/a.mp3', 1: '/b.mp3'}  # picked in Rekordbox, loaded on deck 2
            self.window.sync_load_targets()
            hide.assert_called_once()
            self.assertFalse(self.window.isMinimized())
            self.assertIsNone(self.window.rekordbox_browsing)

    def test_only_the_knob_moving_pulls_the_browser_to_rekordboxs_list(self):
        from rb_waveform_core.mac_bridge import Browser
        state = dict(rows=10, cursor=2, first=0, focus='list', tree_path='Collection', tracks=((1, '/a'),))
        reader = Mock(bridged=True, devices=(), browser=Browser(**state))
        self.session.reader = reader
        self.window.follow_rekordbox_browser()
        self.window.toggle_browser()
        self.window.browser_page.view = ('device', '/Volumes/STICK')  # the user picked their stick
        with patch.object(self.window.browser_page, 'mirror') as mirror:
            reader.browser = Browser(**{**state, 'rows': 12, 'tracks': ((1, '/a'), (2, '/b'))})  # Rekordbox refreshed
            self.window.follow_rekordbox_browser()
            reader.browser = Browser(**{**state, 'rows': 12, 'focus': ''})  # Windows: Wave was touched, Rekordbox lost focus
            self.window.follow_rekordbox_browser()
            mirror.assert_not_called()
            reader.browser = Browser(**{**state, 'rows': 12, 'cursor': 3})  # the knob turned
            self.window.follow_rekordbox_browser()
            mirror.assert_called_once()

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
        with patch('rb_waveform_core.mac_bridge.show') as show, patch.object(self.window, 'activateWindow') as back:
            self.window.watch_rekordbox_start(100)
            self.window.watch_rekordbox_start(105)
            show.assert_not_called()  # an ordinary start
            self.window.watch_rekordbox_start(109)
            self.window.watch_rekordbox_start(120)
            show.assert_called_once()  # waiting on its own questions
            self.assertIn('answer its questions', self.window.status.toolTip())
            self.session.reader.status = 'REKORDBOX · direct'
            self.window.watch_rekordbox_start(121)
            back.assert_called_once()
            self.session.reader.status = 'REKORDBOX · busy'  # stuck later: a question on load, a drive that hangs
            self.window.watch_rekordbox_start(200)
            self.window.watch_rekordbox_start(209)
            self.assertEqual(show.call_count, 2)
            self.assertIn('check your music drive', self.window.status.toolTip())

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
