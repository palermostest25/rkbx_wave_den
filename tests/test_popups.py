"""Rekordbox's notices are closed for the DJ; its questions are never answered for them."""
import time
import unittest

from rb_waveform_core.popups import Popup, PopupWatcher, dismiss_with
from rb_waveform_core.win_windows import Win, classify


class DismissTests(unittest.TestCase):
    def test_a_lone_acknowledging_button_is_pressed(self):
        self.assertEqual(dismiss_with(Popup(1, 'rekordbox', ('The USB device was removed.',), ('OK',))), 'OK')

    def test_later_kinds_of_button_are_pressed(self):
        self.assertEqual(dismiss_with(Popup(1, '', ('A new version is available',), ('Update now', 'Later'))), 'Later')
        self.assertEqual(dismiss_with(Popup(1, '', ('Try rekordbox Creative plan',), ('Learn more', 'Not now'))), 'Not now')

    def test_offers_and_news_may_be_closed_or_cancelled(self):
        self.assertEqual(dismiss_with(Popup(1, "What's new", ('Stems, cloud and more',), ('Learn more', 'Close'))), 'Close')
        self.assertIsNone(dismiss_with(Popup(1, '', ('Analyse 30 tracks?',), ('Analyse', 'Close'))))

    def test_questions_and_anything_unknown_stay_for_the_dj(self):
        for popup in (Popup(1, '', ('Sync your library now?',), ('Sync', 'Cancel')),
                      Popup(1, '', ('Learn more about plans',), ('Learn more',)),  # would open a web page
                      Popup(1, 'Sign in', ('Log in to your account',), ('Later',)),
                      Popup(1, '', ('The database is damaged: restore the backup?',), ('OK',)),
                      Popup(1, '', ('Something',), ())):
            self.assertIsNone(dismiss_with(popup), popup)


class FakeBackend:
    def __init__(self):
        self.showing = []
        self.pressed = []

    def scan(self, pid, exclude):
        return [Popup(p.key, p.title, p.texts, p.buttons) for p in self.showing if p.key not in exclude]

    def press(self, popup, button):
        self.pressed.append((popup.key, button))
        self.showing = [p for p in self.showing if p.key != popup.key]


class WatcherTests(unittest.TestCase):
    def watch(self, backend, seconds):
        watcher = PopupWatcher(backend, interval=.05)
        watcher.pid = 7
        watcher.start()
        time.sleep(seconds)
        return watcher

    def test_notices_are_closed_after_a_moment_and_questions_are_listed(self):
        backend = FakeBackend()
        backend.showing = [Popup('notice', '', ('Update available',), ('Later',)),
                           Popup('question', '', ('Sync now?',), ('Sync', 'Cancel'))]
        watcher = self.watch(backend, 1.2)
        try:
            self.assertEqual(backend.pressed, [('notice', 'Later')])
            self.assertEqual([p.key for p in watcher.popups], ['question'])
            self.assertEqual(len(watcher.closed), 1)
        finally:
            watcher.stop()

    def test_nothing_is_pressed_when_closing_is_off_or_while_the_dj_browses_it(self):
        backend = FakeBackend()
        backend.showing = [Popup('notice', '', ('Update available',), ('Later',)), Popup('browser', '', (), ('OK',))]
        watcher = PopupWatcher(backend, interval=.05)
        watcher.auto_close = False
        watcher.exclude = {'browser'}
        watcher.pid = 7
        watcher.start()
        time.sleep(1)
        try:
            self.assertEqual(backend.pressed, [])
            self.assertEqual([p.key for p in watcher.popups], ['notice'])
        finally:
            watcher.stop()


class ClassifyTests(unittest.TestCase):
    def test_main_window_browser_and_popups(self):
        main = Win(1, 'rekordbox', False, True, (0, 0, 1600, 900), False, True, False)
        split = Win(2, 'rekordbox', True, False, (0, 0, 1200, 800), True, True, False)
        question = Win(3, 'rekordbox', True, False, (500, 300, 900, 500), False, False, False)
        menu = Win(4, '', True, False, (10, 10, 200, 300), False, False, True)
        hidden = Win(5, 'x', False, False, (0, 0, 10, 10), False, False, False)
        found, panels, popups = classify([main, split, question, menu, hidden])
        self.assertEqual((found, panels, popups), (main, [split], [question]))
        _, _, popups = classify([main, question], presenting={3})
        self.assertEqual(popups, [])


if __name__ == '__main__':
    unittest.main()
