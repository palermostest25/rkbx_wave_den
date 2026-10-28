import struct
import unittest
from unittest.mock import Mock, patch
from rb_waveform_core.rb_memory_reader import RekordboxMemoryReader


class MemoryReaderHealthTests(unittest.TestCase):
    def test_unsupported_version_has_actionable_status(self):
        with patch('rb_waveform_core.rb_memory_reader._load_offsets', return_value={'7.2.2': {}}):
            reader = RekordboxMemoryReader(rekordbox_version='9.0.0')
        self.assertFalse(reader._select_offsets(Mock()))
        self.assertIn('Unsupported Rekordbox 9.0.0', reader.status)
        self.assertIn('7.2.2', reader.status)
        self.assertFalse(reader.connected)

    def test_invalid_float_does_not_become_transport_data(self):
        with patch('rb_waveform_core.rb_memory_reader._load_offsets', return_value={}):
            reader = RekordboxMemoryReader()
        for value in (float('nan'), float('inf'), -float('inf')):
            pm = Mock(read_bytes=Mock(return_value=struct.pack('<f', value)))
            self.assertIsNone(reader._read_f32(pm, 0x1000, [], 0x100))
        pm = Mock(read_bytes=Mock(return_value=struct.pack('<f', 128)))
        self.assertEqual(reader._read_f32(pm, 0x1000, [], 0x100), 128)

    def test_process_handle_closes_even_when_stopping(self):
        with patch('rb_waveform_core.rb_memory_reader._load_offsets', return_value={'7.2.2': {}}):
            reader = RekordboxMemoryReader()
        pm = Mock()
        reader._wait_for_rekordbox = Mock(return_value=pm)
        reader._get_module_base = Mock(return_value=0x1000)
        reader._select_offsets = Mock(return_value=True)
        reader._poll_loop = Mock(side_effect=lambda *_: reader._stop_event.set())
        reader._run()
        pm.close_process.assert_called_once()
        self.assertFalse(reader.connected)


if __name__ == '__main__':
    unittest.main()
