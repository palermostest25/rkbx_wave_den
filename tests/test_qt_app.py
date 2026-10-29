"""Native acceptance checks for the single-window UI and physical full-screen bounds."""
import ctypes
import json
import os
import sys
import tempfile
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

    def test_two_and_four_decks_reuse_the_same_window(self):
        handle = self.window.winId()
        self.window.set_count(2)
        self.assertEqual(self.session.order(), (0, 1))
        self.window.set_count(4)
        self.assertEqual(self.session.order(), (2, 0, 1, 3))
        self.assertEqual(handle, self.window.winId())

    def test_selecting_monitor_automatically_fills_entire_panel(self):
        self.window.select_screen(1)
        QTest.qWait(250)
        screen = self.app.screens()[0]
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
        self.assertFalse(self.window.isFullScreen())
        self.window.ensure_display()
        self.assertFalse(self.window.isFullScreen())

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
