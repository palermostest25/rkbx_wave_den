import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import patch
import numpy as np
from rb_waveform_core.ANLZ import _extract_waveform
from rb_waveform_core.analysis import analysis_from_rb_waveform
from rb_waveform_core.config import WaveformColorConfig, WaveformRenderConfig, config_to_dict, dict_to_config
from rb_waveform_core.cues import _cues_from_anlz, _loop_end
from rb_waveform_core.displays import deck_order
from rb_waveform_core.rb_memory_reader import _select_version
from rb_waveform_core.render import render_waveform_image


def tag(name, **content):
    return type(name, (), {'struct': NS(content=NS(**content))})()


class DisplayTests(unittest.TestCase):
    def test_physical_order_and_two_decks(self):
        self.assertEqual(deck_order('3,1,2,4', 4), (2, 0, 1, 3))
        self.assertEqual(deck_order('3,1,2,4', 2), (0, 1))
        for raw in ('1,1,2,3', '0,1,2,3', 'oops', '1,2'):
            with self.assertRaises(ValueError):
                deck_order(raw, 4)

    def test_coincident_cue_flags_never_overlap(self):
        from rb_waveform_core.qt_display import flag_positions
        occupied = flag_positions([100] * 6, 24, 400)
        self.assertEqual(len(set(occupied)), 6)
        self.assertNotIn(None, occupied)

    def test_pwv7_band_order(self):
        waveform = _extract_waveform([tag('PWV7AnlzTag', entries=bytes([20, 5, 100, 40, 9, 80]))])
        np.testing.assert_array_equal(waveform, [[100, 20, 5], [80, 40, 9]])

    def test_legacy_packed_height_is_not_fake_three_band(self):
        waveform = _extract_waveform([tag('PWV5AnlzTag', entries=[0xffff, 0xe040])])
        np.testing.assert_array_equal(waveform, [[31, 31, 31], [16, 16, 16]])

    def test_preview_is_not_used_as_150fps_detail(self):
        self.assertIsNone(_extract_waveform([tag('PWV6AnlzTag', entries=b'\1\2\3')]))

    def test_normalization_preserves_relative_band_energy(self):
        analysis = analysis_from_rb_waveform(np.array([[100, 20, 5], [80, 40, 9]]), 1)
        self.assertAlmostEqual(float(analysis.mid[0]), 20 * .75 / 127)
        self.assertAlmostEqual(float(analysis.high[0]), 5 * .15 / 127)

    def test_loop_sentinels(self):
        for end in (None, -1, 0xffffffff, 900, 1000):
            self.assertIsNone(_loop_end(1000, end))
        self.assertEqual(_loop_end(1000, 2500), 2.5)

    def test_scan_all_files_prefer_extended_and_keep_same_time_slots(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            for suffix in ('DAT', 'EXT', '2EX'):
                (folder / ('ANLZ.' + suffix)).touch()
            basic = NS(time=1000, hot_cue=1, type='single', loop_time=0xffffffff)
            richer = NS(time=1000, hot_cue=1, type=2, loop_time=3000,
                        color_red=15, color_green=200, color_blue=125)
            other = NS(time=1000, hot_cue=2, type=1)
            def parse(path):
                tags = [tag('PCO2AnlzTag', entries=[richer, other])] if path.endswith('EXT') else [tag('PCOBAnlzTag', entries=[basic])]
                return NS(tags=tags)
            with patch('rb_waveform_core.cues.AnlzFile.parse_file', side_effect=parse):
                cues = _cues_from_anlz(folder)
            self.assertEqual(len(cues), 2)
            self.assertEqual(cues[0].color, (15, 200, 125))
            self.assertEqual(cues[0].end_sec, 3)

    def test_settings_roundtrip_and_legacy_defaults(self):
        render = WaveformRenderConfig(display_key='portable', fill_display=True, ui_scale=1.25)
        _, restored = dict_to_config(config_to_dict(WaveformColorConfig(), render))
        self.assertEqual(restored, render)
        self.assertEqual(dict_to_config({'render': {'deck_count': 2}})[1].deck_order, '3,1,2,4')
        with self.assertRaises(ValueError):
            dict_to_config({'render': {'deck_count': 99}})

    def test_unknown_memory_version_rejected(self):
        self.assertEqual(_select_version('7.2.2.0', ['7.2.2', '7.2.14']), '7.2.2')
        self.assertIsNone(_select_version('9.0.0', ['7.2.2', '7.2.14']))
        self.assertIsNone(_select_version(None, ['7.2.2']))

    def test_three_band_overlap_pixels(self):
        analysis = analysis_from_rb_waveform(np.array([[100, 60, 20]]), 1)
        img = render_waveform_image(analysis, WaveformColorConfig(), WaveformRenderConfig(image_width=20, image_height=100))
        self.assertEqual(img.getpixel((10, 50)), (255, 255, 255))
        self.assertEqual(img.getpixel((10, 60)), (169, 107, 39))
        self.assertEqual(img.getpixel((10, 82)), (0, 120, 255))


if __name__ == '__main__':
    unittest.main()
