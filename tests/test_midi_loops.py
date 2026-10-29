import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from PySide6.QtWidgets import QApplication

from rb_waveform_core.midi_loops import MidiLoopListener, find_map, load_map, messages
from rb_waveform_core.qt_app import Session

FLX10 = """@file,1,DDJ-FLX10,,,,,,,,,,,,
#name,function,type,input,deck1,deck2,deck3,deck4,output,deck1,deck2,deck3,deck4,option,comment
LoopIn,LoopIn,Button,9010,0,1,2,3,9010,0,1,2,3,Dual;Fast,Loop in/Loop in adjust
LoopOut,LoopOut,Button,9011,0,1,2,3,9011,0,1,2,3,Dual;Fast,Loop out/Loop out adjust
ReloopExit,LoopOut+Shift,Button,904D,0,1,2,3,904D,0,1,2,3,Dual;Fast,Reloop/Exit
4BeatLoop,4BeatLoop,Button,9014,0,1,2,3,9014,0,1,2,3,Dual;Fast,Auto Loop On/Off
PAD5_BeatLoop,PAD5_PadMode2,Pad,9060,7,9,,,9060,7,9,,,Fast,
PadMode1,HotCueMode,Button,901B,0,1,2,3,901B,0,1,2,3,,HOT CUE Mode On/Off
PadMode2,PadFx1Mode,Button,901E,0,1,2,3,901E,0,1,2,3,,PAD FX1 Mode On/Off
PAD1_PadMode1,PAD1_PadMode1,Pad,9000,7,9,11,13,9000,7,9,11,13,Fast;Dual,HOT CUE1
PAD3_PadMode1+Shift,PAD3_PadMode1+Shift,Pad,9002,8,10,12,14,9002,8,10,12,14,Fast;Dual,Delete HOT CUE3
PAD1_PadMode2,PAD1_PadMode2,Pad,9010,7,9,11,13,9010,7,9,11,13,Fast;Dual,PAD FX1 Slot1
Memory,Memory,Button,903D,0,1,2,3,903D,0,1,2,3,,Memory
MidiOutInterval,,,,,,,,,,,,,,
GetLoopActive,LoopState,Indicator,,,,,,,9F04,9F05,9F06,9F07,RO;Priority=100,
"""


class MidiMapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'DDJ-FLX10.midi.csv'
        self.path.write_text(FLX10)

    def tearDown(self):
        self.temp.cleanup()

    def test_rekordbox_map_decodes_loop_buttons_per_deck(self):
        name, table = load_map(self.path)
        self.assertEqual(name, 'DDJ-FLX10')
        self.assertEqual(table[0x90, 0x10], (0, ('in', None)))
        self.assertEqual(table[0x93, 0x11], (3, ('out', None)))
        self.assertEqual(table[0x91, 0x4D], (1, ('reloop', None)))
        self.assertEqual(table[0x99, 0x60], (1, ('auto', .25)))
        self.assertEqual(table[0x97, 0x00], (0, ('hotcue', 1)))  # pad modes resolved by their label
        self.assertEqual(table[0x9C, 0x02], (2, ('delete', 3)))
        self.assertNotIn((0x97, 0x10), table)  # pad FX
        self.assertEqual(table[0x92, 0x3D], (2, ('memory', None)))

    def test_device_picks_the_most_specific_map(self):
        maps = [('PIONEER DDJ-SX', {1: 1}), ('PIONEER DDJ-SX2', {2: 2}), ('XDJ-XZ (1)', {3: 3})]
        self.assertEqual(find_map('DDJ-SX2', maps)[0], 'PIONEER DDJ-SX2')
        self.assertEqual(find_map('Pioneer DDJ-SX', maps)[0], 'PIONEER DDJ-SX')
        self.assertEqual(find_map('XDJ-XZ', maps)[0], 'XDJ-XZ (1)')
        self.assertIsNone(find_map('IAC Driver Bus 1', maps))

    def test_byte_stream_handles_running_status_and_sysex(self):
        data = bytes([0xF0, 0x00, 0x40, 0xF7, 0x90, 0x10, 0x7F, 0x11, 0x00, 0xF8, 0xB1, 0x07, 0x40])
        self.assertEqual(list(messages(data)), [(0x90, 0x10, 0x7F), (0x90, 0x11, 0x00), (0xB1, 0x07, 0x40)])

    def test_only_presses_become_loop_events(self):
        listener = MidiLoopListener()
        listener.maps = [load_map(self.path)]
        listener._attach(5, 'DDJ-FLX10')
        listener._receive(5, bytes([0x90, 0x10, 0x7F, 0x90, 0x10, 0x00, 0x80, 0x11, 0x40]))
        deck, kind, beats, _ = listener.get_event()
        self.assertEqual((deck, kind, beats), (0, 'in', None))
        self.assertIsNone(listener.get_event())
        self.assertEqual(listener.controllers, ['DDJ-FLX10'])


class LoopActionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.session = Session(demo=True, config_path=Path(self.temp.name) / 'settings.json')
        # 120 BPM: a beat every half second.
        self.session.decks[0] = SimpleNamespace(
            original_bpm=120.0, cues=[],
            beat_grid_full=[SimpleNamespace(time_ms=i * 500) for i in range(600)])
        self.timeline = self.session.timelines[0]
        self.timeline.observe(10.0, observed_at=100.0)
        self.timeline.observe(10.5, observed_at=100.5)

    def tearDown(self):
        self.session.close()
        self.temp.cleanup()

    def press(self, kind, at, beats=None):
        self.session.loop_action(0, kind, beats, at)

    def test_in_shows_marker_and_out_engages_at_once(self):
        self.press('in', 100.5)
        self.assertEqual(self.timeline.pending_in, 10.5)
        self.assertIsNone(self.timeline.active_loop())
        self.timeline.observe(11.95, observed_at=101.95)
        self.press('out', 101.95)
        self.assertEqual(self.timeline.active_loop(), (10.5, 12.0))
        self.assertIsNone(self.timeline.pending_in)

    def test_auto_loop_toggles_and_reloop_restores(self):
        self.press('auto', 100.5)
        self.assertEqual(self.timeline.active_loop(), (10.5, 12.5))
        self.press('scale', 100.5, .5)
        self.assertEqual(self.timeline.active_loop(), (10.5, 11.5))
        self.press('auto', 100.5)
        self.assertIsNone(self.timeline.active_loop())
        self.press('reloop', 100.5)
        self.assertEqual(self.timeline.active_loop(), (10.5, 11.5))
        self.press('reloop', 100.5)
        self.assertIsNone(self.timeline.active_loop())

    def test_loop_wraps_without_waiting_for_a_repeat(self):
        self.press('auto', 100.5, 1)
        self.timeline.observe(10.9, observed_at=100.9)
        self.timeline.observe(10.6, observed_at=101.1)  # rekordbox wrapped back to the start
        self.assertEqual(self.timeline.active_loop(), (10.5, 11.0))
        self.assertGreater(self.timeline.rate, 0)

    def test_cue_presses_show_at_once_and_master_db_stays_in_charge(self):
        from rb_waveform_core.cues import CueMarker
        cues = lambda: [(c.number, c.time_sec, c.is_memory) for c in self.session.decks[0].cues]
        self.press('hotcue', 100.5, 2)
        self.assertEqual(cues(), [(2, 10.5, False)])
        self.press('memory', 100.5)
        self.assertEqual(cues(), [(0, 10.5, True), (2, 10.5, False)])
        self.session.apply_cues(0, [])  # master.db not written yet: keep showing the presses
        self.assertEqual(len(cues()), 2)
        db = [CueMarker(10.52, (1, 1, 1), False, 2), CueMarker(10.5, (1, 1, 1), True, 0)]
        self.session.apply_cues(0, db)
        self.assertEqual(self.session.cue_edits[0], [])
        self.press('delete', 100.6, 2)
        self.assertEqual(cues(), [(0, 10.5, True)])
        self.session.apply_cues(0, db)  # stale read still has the cue
        self.assertEqual(cues(), [(0, 10.5, True)])
        self.session.apply_cues(0, db[1:])
        self.assertEqual(self.session.cue_edits[0], [])
        self.session.apply_cues(0, [])  # deleted with the mouse
        self.assertEqual(cues(), [])

    def test_exit_by_mouse_clears_when_playhead_leaves(self):
        self.press('auto', 100.5, 1)
        self.timeline.observe(11.0, observed_at=101.0)
        self.timeline.observe(11.5, observed_at=101.5)
        self.assertIsNone(self.timeline.active_loop())


if __name__ == '__main__':
    unittest.main()
