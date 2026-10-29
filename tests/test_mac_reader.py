import ctypes
import plistlib
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from rb_waveform_core.mac_reader import (MachProcess, MacMemoryAccessError,
    MAC_LAYOUTS, RekordboxMacMemoryReader, app_version)


def fake_lib():
    lib = Mock()
    lib.mach_task_self.return_value = 99
    def attach(_self, _pid, output):
        ctypes.cast(output, ctypes.POINTER(ctypes.c_uint32))[0] = 77
        return 0
    lib.task_for_pid.side_effect = attach
    return lib


class MacReaderTests(unittest.TestCase):
    def test_denied_access_is_actionable(self):
        lib = fake_lib()
        lib.task_for_pid.side_effect = None
        lib.task_for_pid.return_value = 5
        with self.assertRaisesRegex(MacMemoryAccessError, 'denied read access'):
            MachProcess(123, lib)

    def test_read_range_short_reads_and_port_lifecycle(self):
        lib = fake_lib()
        def read(_task, _address, size, output, count):
            ctypes.memmove(output, b'abcd', size)
            ctypes.cast(count, ctypes.POINTER(ctypes.c_uint64))[0] = size
            return 0
        lib.mach_vm_read_overwrite.side_effect = read
        process = MachProcess(123, lib)
        self.assertEqual(process.read_bytes(0x1000, 4), b'abcd')
        for address, size in ((0, 4), (1, 0), (1, 2**24)):
            with self.assertRaises(ValueError):
                process.read_bytes(address, size)
        lib.mach_vm_read_overwrite.side_effect = lambda *args: 1
        with self.assertRaises(MacMemoryAccessError):
            process.read_bytes(0x1000, 4)
        process.close_process()
        process.close_process()
        lib.mach_port_deallocate.assert_called_once_with(99, 77)
        with self.assertRaises(MacMemoryAccessError):
            process.read_bytes(0x1000, 4)

    def test_main_image_base_is_validated(self):
        lib = fake_lib()
        process = MachProcess(123, lib)
        def metadata(_task, flavor, output, count):
            from rb_waveform_core.mac_reader import _DyldInfo
            self.assertEqual(flavor, 17)
            ctypes.cast(output, ctypes.POINTER(_DyldInfo))[0].address = 0x1000
            return 0
        lib.task_info.side_effect = metadata
        reads = {0x1000: struct.pack('<IIQ', 1, 50, 0x2000),
                 0x2000: struct.pack('<Q', 0x100000000),
                 0x100000000: b'\xcf\xfa\xed\xfe'}
        process.read_bytes = Mock(side_effect=lambda address, size: reads[address])
        self.assertEqual(process.module_base(), 0x100000000)
        reads[0x100000000] = b'bad!'
        with self.assertRaises(MacMemoryAccessError):
            process.module_base()
        process.close_process()

    def test_versions_and_architectures_fail_closed(self):
        reader = RekordboxMacMemoryReader()
        self.assertTrue(reader._choose_layout('7.2.18.0342', 'arm64'))
        self.assertIs(reader._offsets, MAC_LAYOUTS['7.2.18'])
        self.assertFalse(reader._choose_layout('7.2.19.0342', 'arm64'))
        self.assertIn('No Mac memory layout', reader.status)
        self.assertFalse(reader._choose_layout('7.2.18', 'x86_64'))
        self.assertIn('Intel', reader.status)
        self.assertFalse(reader._choose_layout(None, 'arm64'))

    def test_native_polling_emits_real_position_and_path(self):
        reader = RekordboxMacMemoryReader(deck_count=2)
        reader._choose_layout('7.2.18', 'arm64')
        reader._read_f32 = Mock(return_value=128.0)
        reader._read_i64 = Mock(return_value=441000)
        reader._read_anlz_path = Mock(return_value='/USB/PIONEER/USBANLZ/P001/test')
        reader._stop_event.wait = Mock(side_effect=lambda seconds: reader._stop_event.set())
        reader._poll_loop(Mock(), 0x100000000)
        events = []
        while (event := reader.get_event(timeout=0)) is not None:
            events.append(event)
        self.assertEqual(len(events), 4)
        self.assertEqual(events[0].time_seconds, 10)
        self.assertEqual(events[-1].anlz_path, '/USB/PIONEER/USBANLZ/P001/test')

    def test_authorized_copy_is_preferred_and_explicit_target_does_not_fallback(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            targets = []
            for pid, name, identifier in ((10, 'Original', 'com.pioneerdj.rekordboxdj'),
                                           (20, 'Calibration', 'dev.rekordbox-wave.calibration')):
                contents = root / (name + '.app') / 'Contents'
                (contents / 'MacOS').mkdir(parents=True)
                (contents / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': identifier}))
                targets.append(Mock(info={'pid': pid, 'name': 'rekordbox', 'exe': str(contents/'MacOS'/'rekordbox')}))
            reader = RekordboxMacMemoryReader()
            with patch('psutil.process_iter', return_value=targets):
                self.assertEqual(reader._find_process()[0], 20)
            reader = RekordboxMacMemoryReader(preferred_executable=targets[0].info['exe'])
            with patch('psutil.process_iter', return_value=targets):
                self.assertEqual(reader._find_process()[0], 10)
            reader = RekordboxMacMemoryReader(preferred_executable=root/'missing.app'/'Contents'/'MacOS'/'rekordbox')
            with patch('psutil.process_iter', return_value=targets):
                self.assertIsNone(reader._find_process())

    def test_version_is_read_from_the_correct_app_bundle(self):
        with tempfile.TemporaryDirectory() as root:
            contents = Path(root) / 'test.app' / 'Contents'
            (contents / 'MacOS').mkdir(parents=True)
            (contents / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleShortVersionString':'7.2.18.0342'}))
            self.assertEqual(app_version(contents/'MacOS'/'rekordbox'), '7.2.18.0342')
