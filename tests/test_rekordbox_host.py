"""Rekordbox kept started, connected and running behind Wave, never at the cost of a playing deck."""
import struct
import time
import unittest
from unittest.mock import Mock

from rb_waveform_core.rekordbox_host import (ATTACH, ASK_CONNECT, ASK_GAVE_UP, ASK_START, LAUNCH, RESTART, HostPolicy,
                                             RekordboxHost, Seen)
from rb_waveform_core.win_attach import AttachError, export_rva


def mac(**kw):
    return HostPolicy(windows=False, **kw)


def win(**kw):
    return HostPolicy(windows=True, **kw)


class HostPolicyTests(unittest.TestCase):
    def test_starts_rekordbox_when_wave_starts_and_it_isnt_running(self):
        policy = mac()
        self.assertEqual(policy.decide(Seen(None), 0), LAUNCH)
        self.assertIsNone(policy.decide(Seen(None), 5))  # starting: no second launch
        self.assertIsNone(policy.decide(Seen(7, 'none', age=3), 10))  # its bridge arrives with OpenSSL's config
        self.assertIsNone(policy.decide(Seen(7, 'starting', age=8), 15))
        self.assertIsNone(policy.decide(Seen(7, 'live', age=20), 25))
        self.assertIsNone(policy.ask)

    def test_never_starts_rekordbox_when_told_not_to(self):
        policy = mac(auto_start=False)
        self.assertIsNone(policy.decide(Seen(None), 0))
        self.assertEqual(policy.ask, ASK_START)

    def test_windows_connects_live_to_a_rekordbox_started_without_wave(self):
        policy = win()
        self.assertEqual(policy.decide(Seen(7, 'none', playing=True, loaded=True, known=True, age=3600), 0), ATTACH)
        # Tried once: then never restarted under a playing deck.
        self.assertIsNone(policy.decide(Seen(7, 'none', playing=True, loaded=True, known=True, age=3600), 1))
        self.assertEqual(policy.ask, ASK_CONNECT)

    def test_windows_never_loads_a_second_bridge(self):
        policy = win()
        self.assertNotEqual(policy.decide(Seen(7, 'none', bridge_loaded=True, known=True, playing=True, age=999), 0), ATTACH)

    def test_windows_connects_live_when_its_own_start_brought_no_bridge(self):
        policy = win()
        self.assertEqual(policy.decide(Seen(None), 0), LAUNCH)
        self.assertIsNone(policy.decide(Seen(7, 'none', age=10), 10))
        self.assertEqual(policy.decide(Seen(7, 'none', age=35), 35), ATTACH)

    def test_mac_restarts_an_idle_rekordbox_started_without_wave(self):
        policy = mac()
        policy.first = False
        self.assertEqual(policy.decide(Seen(7, 'none', known=True, age=600), 0), RESTART)
        self.assertIsNone(policy.decide(Seen(8, 'none', age=2), 1))  # the restarted one, starting

    def test_a_fresh_rekordbox_is_restarted_even_unseen(self):
        policy = mac()
        policy.first = False
        self.assertEqual(policy.decide(Seen(7, 'none', known=False, age=20), 0), RESTART)

    def test_a_rekordbox_wave_cant_see_into_is_never_restarted_alone(self):
        policy = mac()
        policy.first = False
        self.assertIsNone(policy.decide(Seen(7, 'none', known=False, age=900), 0))
        self.assertEqual(policy.ask, ASK_CONNECT)

    def test_a_loaded_or_playing_rekordbox_waits_for_the_dj(self):
        for seen in (Seen(7, 'none', known=True, loaded=True, age=900), Seen(7, 'older', known=True, playing=True, age=900),
                     Seen(7, 'none', known=False, loaded=True, age=20)):
            policy = mac()
            policy.first = False
            self.assertIsNone(policy.decide(seen, 0), seen)
            self.assertEqual(policy.ask, ASK_CONNECT)

    def test_an_idle_rekordbox_with_an_earlier_waves_bridge_is_restarted_once(self):
        policy = mac()
        self.assertEqual(policy.decide(Seen(7, 'live', stale=True, known=True, age=900), 0), RESTART)
        policy.launched(8)
        self.assertIsNone(policy.decide(Seen(8, 'live', stale=True, known=True, age=5), 30))  # not again and again
        policy = mac()
        self.assertIsNone(policy.decide(Seen(7, 'live', stale=True, known=True, loaded=True, age=900), 0))
        self.assertIn('earlier Wave', policy.ask)  # works meanwhile; CONNECT finishes the update

    def test_rekordbox_closing_under_wave_is_started_again_with_a_limit(self):
        policy = mac()
        policy.decide(Seen(7, 'live'), 0)
        self.assertEqual(policy.decide(Seen(None), 10), LAUNCH)
        for n, start in enumerate((200, 400)):
            policy.decide(Seen(10 + n, 'live'), start)
            self.assertEqual(policy.decide(Seen(None), start + 10), LAUNCH)
        policy.decide(Seen(20, 'live'), 450)
        self.assertIsNone(policy.decide(Seen(None), 460))  # three in ten minutes: crashing, stop
        self.assertEqual(policy.ask, ASK_GAVE_UP)

    def test_rekordbox_quit_by_wave_is_not_started_again(self):
        policy = mac()
        policy.decide(Seen(7, 'live'), 0)
        policy.quitting = True
        self.assertIsNone(policy.decide(Seen(None), 10))

    def test_rekordbox_wave_never_used_is_not_started_again(self):
        policy = mac(auto_start=False)
        policy.decide(Seen(7, 'unsupported'), 0)
        self.assertIsNone(policy.decide(Seen(None), 10))

    def test_unsupported_builds_are_left_alone(self):
        policy = win()
        self.assertIsNone(policy.decide(Seen(7, 'unsupported', age=999), 0))
        self.assertIsNone(policy.ask)


class HostTests(unittest.TestCase):
    def test_actions_run_off_the_ui_thread_one_at_a_time_and_report_failures(self):
        launched = []
        host = RekordboxHost(mac(), launch=lambda: (time.sleep(.05), launched.append(1)), restart=Mock(),
                             attach=Mock(), pid=lambda: 7)
        self.assertEqual(host.poll(Seen(None), 0), LAUNCH)
        self.assertEqual(host.doing, 'Starting Rekordbox')
        self.assertIsNone(host.poll(Seen(None), 1))  # busy
        host.job.result()
        host.poll(Seen(7, 'live'), 2)
        self.assertEqual((launched, host.doing, host.error), ([1], '', ''))
        self.assertIn(7, host.policy.owned)
        host.close()

    def test_a_failed_live_connect_is_reported(self):
        host = RekordboxHost(win(), launch=Mock(), restart=Mock(), attach=lambda pid: (False, 'nope'), pid=lambda: 7)
        host.policy.first = False
        self.assertEqual(host.poll(Seen(7, 'none', age=999, known=True, playing=True), 0), ATTACH)
        host.job.exception()
        host.poll(Seen(7, 'none', age=999, known=True, playing=True), 1)
        self.assertEqual(host.error, 'nope')
        host.close()

    def test_quits_only_a_rekordbox_wave_used_and_never_mid_play(self):
        host = RekordboxHost(mac(), launch=Mock(), restart=Mock(), attach=Mock(), pid=lambda: 7)
        self.assertFalse(host.quit())  # not Wave's
        host.policy.owned.add(7)
        self.assertFalse(host.quit(playing=True))
        host.close()


def portable_executable(exports):
    """A minimal PE32+ image exporting ``exports`` (name -> rva)."""
    image = bytearray(0x400)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x40)
    image[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HH12xHH', image, 0x44, 0x8664, 1, 240, 0x2022)
    optional = 0x58
    struct.pack_into('<H', image, optional, 0x20b)
    struct.pack_into('<II', image, optional + 112, 0x1000, 0x100)  # the export directory
    section = optional + 240
    image[section:section + 8] = b'.edata\0\0'
    struct.pack_into('<IIII', image, section + 8, 0x200, 0x1000, 0x200, 0x200)
    raw = lambda rva: 0x200 + rva - 0x1000
    names = sorted(exports)
    struct.pack_into('<IIIII', image, raw(0x1000) + 20, len(names), len(names), 0x1040, 0x1060, 0x1080)
    at = 0x10a0
    for i, name in enumerate(names):
        struct.pack_into('<I', image, raw(0x1040) + 4 * i, exports[name])
        struct.pack_into('<I', image, raw(0x1060) + 4 * i, at)
        struct.pack_into('<H', image, raw(0x1080) + 2 * i, i)
        image[raw(at):raw(at) + len(name) + 1] = name.encode() + b'\0'
        at += 0x20
    return bytes(image)


class ExportTests(unittest.TestCase):
    def test_finds_the_attach_entry_by_name(self):
        image = portable_executable({'OSSL_provider_init': 0x3010, 'wave_bridge_attach': 0x3070})
        self.assertEqual(export_rva(image), 0x3070)
        self.assertEqual(export_rva(image, 'OSSL_provider_init'), 0x3010)

    def test_an_earlier_bridge_without_it_is_refused(self):
        with self.assertRaisesRegex(AttachError, 'predates'):
            export_rva(portable_executable({'OSSL_provider_init': 0x3010}))
        with self.assertRaises(AttachError):
            export_rva(b'not a library')


if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(__import__('sys').platform == 'win32', 'Windows live connecting')
class LiveAttachOnWindowsTests(unittest.TestCase):
    """The whole live connect - open the process, load the bridge, find and run its entry - on a stand-in
    process: the bridge looks, sees it is not Rekordbox, and says so without doing anything else."""
    def test_the_bridge_is_loaded_and_answers_in_another_process(self):
        import subprocess, sys
        from pathlib import Path
        from rb_waveform_core.win_attach import attach, module_base
        library = Path(__file__).resolve().parents[1] / 'rb_waveform_core' / 'data' / 'wave-bridge.dll'
        if not library.exists():
            self.skipTest('the Windows bridge is not built here')
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        try:
            time.sleep(1)
            self.assertEqual(attach(child.pid, library), (False, 'That process is not Rekordbox'))
            self.assertTrue(module_base(child.pid, library))
            self.assertIsNone(child.poll())  # and the process carries on
        finally:
            child.kill()
