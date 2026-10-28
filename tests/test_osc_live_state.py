import math
import unittest
from rb_waveform_core.rkbx_link_listener import DeckState, RekordboxLinkListener


class OscLiveStateTests(unittest.TestCase):
    def setUp(self):
        self.reader = RekordboxLinkListener(decks=(0, 1))

    def event(self):
        return self.reader.get_event(timeout=0)

    def test_model_preserves_optional_transport_fields(self):
        state = DeckState(0, playing=True, rate=1.1, loop_start=4, loop_end=8, loop_active=True)
        event = state.snapshot('test', when=10)
        self.assertTrue(event.playing)
        self.assertEqual((event.rate, event.loop_start, event.loop_end, event.loop_active), (1.1, 4, 8, True))
        self.assertIsNone(DeckState(1).snapshot('test').loop_active)

    def test_live_loop_enable_and_disable_are_atomic_snapshots(self):
        self.reader._handle_loop('/loop/0', 4, 8, 1)
        event = self.event()
        self.assertEqual((event.loop_start, event.loop_end, event.loop_active), (4, 8, True))
        self.reader._handle_loop('/loop/0', 0, 0, 0)
        disabled = self.event()
        self.assertEqual((disabled.loop_start, disabled.loop_end, disabled.loop_active), (None, None, False))
        self.assertEqual((event.loop_start, event.loop_end, event.loop_active), (4, 8, True))

    def test_invalid_loop_messages_do_not_change_state(self):
        for args in ((4, 4, True), (-1, 4, True), (4, 8, 'false'), (4, 8, 2),
                     (math.nan, 8, True), (4, math.inf, False), (4, 8), ('bad', 8, True)):
            self.reader._handle_loop('/loop/0', *args)
            self.assertIsNone(self.event(), args)
        self.reader._handle_loop('/loop/99', 4, 8, True)
        self.assertIsNone(self.event())

    def test_new_track_and_unload_reset_live_state(self):
        self.reader._handle_anlz_path('/track/0/anlz_path', '/track-a/ANLZ0000.DAT')
        self.event()
        self.reader._handle_loop('/loop/0', 4, 8, True)
        self.event()
        self.reader._handle_time('/time/0', 6)
        self.event()
        self.reader._handle_anlz_path('/track/0/anlz_path', '/track-b/ANLZ0000.DAT')
        event = self.event()
        self.assertEqual(event.anlz_path, '/track-b')
        self.assertIsNone(event.loop_active)
        self.assertIsNone(event.time_seconds)
        self.reader._handle_anlz_path('/track/0/anlz_path', '')
        self.assertIsNone(self.event().anlz_path)

    def test_playing_and_reverse_rate(self):
        self.reader._handle_playing('/playing/0', True)
        self.assertTrue(self.event().playing)
        self.reader._handle_rate('/rate/0', -.5)
        self.assertEqual(self.event().rate, -.5)
        self.reader._handle_playing('/playing/0', 0)
        self.assertFalse(self.event().playing)
        for handler, address, args in ((self.reader._handle_rate, '/rate/0', (math.nan,)),
                                       (self.reader._handle_rate, '/rate/0', (5,)),
                                       (self.reader._handle_playing, '/playing/0', ('false',))):
            handler(address, *args)
            self.assertIsNone(self.event())

    def test_invalid_numeric_inputs_are_ignored(self):
        for handler, address in ((self.reader._handle_time, '/time/0'),
                                 (self.reader._handle_bpm_current, '/bpm/0/current'),
                                 (self.reader._handle_bpm_original, '/bpm/0/original')):
            for args in ((), ('bad',), (math.nan,), (math.inf,), (1, 2)):
                handler(address, *args)
                self.assertIsNone(self.event())


if __name__ == '__main__':
    unittest.main()
