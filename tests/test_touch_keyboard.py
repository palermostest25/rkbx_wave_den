"""Windows' touch keyboard: off while Wave runs, as it was when Wave closes, even after a crash."""
import unittest

from rb_waveform_core import touch_keyboard as tk


class FakeRegistry:
    def __init__(self, **values):
        self.values = dict(values)
        self.announced = 0

    def get(self, name):
        return self.values.get(name)

    def set(self, name, value):
        self.values[name] = value

    def announce(self):
        self.announced += 1


class TouchKeyboardTests(unittest.TestCase):
    def test_off_while_wave_runs_and_back_after(self):
        registry, view, saves = FakeRegistry(TouchKeyboardTapInvoke=tk.NO_KEYBOARD), {}, []
        guard = tk.Guard(view, lambda: saves.append(dict(view)), registry)
        guard.start()
        self.assertEqual(tk.mode(registry), tk.NEVER)
        self.assertEqual(registry.values['EnableDesktopModeAutoInvoke'], 0)  # Windows 10's switch too
        self.assertEqual(saves[0]['touch_keyboard_restore'], tk.NO_KEYBOARD)  # kept before changing it
        guard.stop()
        self.assertEqual(tk.mode(registry), tk.NO_KEYBOARD)
        self.assertIsNone(view['touch_keyboard_restore'])

    def test_a_crashed_wave_restores_the_real_setting_next_time(self):
        registry = FakeRegistry(TouchKeyboardTapInvoke=tk.ALWAYS)
        view = {}
        tk.Guard(view, lambda: None, registry).start()  # ...and Wave crashed here
        guard = tk.Guard(view, lambda: None, registry)
        guard.start()  # the next start must not take Never as the DJ's own setting
        guard.stop()
        self.assertEqual(tk.mode(registry), tk.ALWAYS)

    def test_set_by_hand_and_left_that_way(self):
        registry, view = FakeRegistry(TouchKeyboardTapInvoke=tk.NO_KEYBOARD), {}
        guard = tk.Guard(view, lambda: None, registry)
        guard.start()
        guard.choose(tk.NO_KEYBOARD)
        self.assertEqual(tk.mode(registry), tk.NO_KEYBOARD)
        guard.choose(tk.NEVER)
        guard.stop()
        self.assertEqual(tk.mode(registry), tk.NEVER)  # what the DJ picked last

    def test_switched_off_wave_leaves_it_alone(self):
        registry = FakeRegistry(TouchKeyboardTapInvoke=tk.NO_KEYBOARD)
        tk.Guard({'touch_keyboard_off': False}, lambda: None, registry).start()
        self.assertEqual(tk.mode(registry), tk.NO_KEYBOARD)

    def test_windows_10_and_unset_settings_read_sensibly(self):
        self.assertEqual(tk.mode(FakeRegistry()), tk.NO_KEYBOARD)
        self.assertEqual(tk.mode(FakeRegistry(EnableDesktopModeAutoInvoke=0)), tk.NEVER)


if __name__ == '__main__':
    unittest.main()
