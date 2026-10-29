import unittest
from types import SimpleNamespace as N
from rb_waveform_core.mac_ax import AXText
from rb_waveform_core.mac_accessibility_reader import deck_fields, seconds, RekordboxAccessibilityReader


def node(value='',role='AXStaticText',help=''):
    return AXText(0,role,value,'',help)


class MacAccessibilityTests(unittest.TestCase):
    def test_four_decks_are_mapped_and_clocks_remain_separate(self):
        nodes=[]
        for title in ('Not Loaded.','Second','Third','Fourth'):
            nodes += [node(title),node(role='AXImage'),node('Artist'),node('128'),
                      node('7A',help='Key:\nDisplays the key'),node(role='AXGroup'),
                      node('-02:00'),node('.5'),node('01:00'),node('.2'),
                      node('MASTER',role='AXButton',help='Sync Master:\nSet master')]
        fields=deck_fields(nodes)
        self.assertEqual([f.deck for f in fields],[0,1,2,3])
        self.assertEqual([f.title.text for f in fields],['Not Loaded.','Second','Third','Fourth'])
        self.assertEqual(fields[3].elapsed.text,'01:00')
        self.assertEqual(fields[3].remaining.text,'-02:00')
        self.assertEqual(seconds('1:02:03'),3723)
        self.assertEqual(seconds('-00:02'),-2)

    def test_partial_or_unsupported_layout_is_not_guessed(self):
        self.assertEqual(deck_fields([node('Key:',help='Key:'),node('00:01')]),[])

    def test_clock_rollover_is_read_consistently(self):
        values=iter(['00:59','.0','01:00','01:00','.1','01:00'])
        ax=N(text=lambda _:next(values))
        self.assertAlmostEqual(RekordboxAccessibilityReader._clock(ax,N(element=1),N(element=2)),60.1)

    def test_malformed_clock_does_not_become_a_seek(self):
        ax=N(text=lambda _:'bad')
        with self.assertRaises(ValueError):
            RekordboxAccessibilityReader._clock(ax,N(element=1),N(element=2))
