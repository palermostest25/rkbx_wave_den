import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from rb_waveform_core.mac_copy import _paths, isolation_policy, prepare_rekordbox_copy


class MacCopyTests(unittest.TestCase):
    def test_source_copy_and_profile_cannot_overlap(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder)/'vendor.app'
            binary = source/'Contents/MacOS/rekordbox'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'vendor')
            with self.assertRaises(ValueError):
                _paths(source, source/'nested.app', Path(folder)/'profile')
            with self.assertRaises(ValueError):
                _paths(source, Path(folder)/'copy.app', source/'profile')
            with self.assertRaises(ValueError):
                _paths(source, Path(folder)/'copy.app', Path(folder))

    def test_sandbox_protects_library_but_allows_normal_track_reads(self):
        home = Path('/Users/example')
        policy = isolation_policy(home/'Library/Application Support/Rekordbox Wave/profile', home)
        self.assertIn('(deny file-write*)', policy)
        self.assertIn('/Users/example/Library/Pioneer', policy)
        self.assertIn('/Users/example/Library/Application Support/Pioneer', policy)
        self.assertNotIn('/Users/example/Music', policy)
        self.assertNotIn('deny network', policy)
        staged = isolation_policy(home/'Wave/profile', home, Path('/private/var/folders/test/T'))
        self.assertIn('NSIRD_rekordbox_', staged)
        self.assertIn('(deny file-write*)', staged)
        self.assertNotIn('(subpath \"/private/var/folders/test/T\")', staged)
        with self.assertRaises(ValueError):
            isolation_policy(home/'Library/Pioneer/profile', home)

    def test_existing_nonempty_profile_is_never_reused_for_new_copy(self):
        import plistlib
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'vendor.app'
            binary = source/'Contents/MacOS/rekordbox'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'vendor')
            (source/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleShortVersionString':'7.2.19.0342', 'CFBundleIdentifier':'com.pioneerdj.rekordboxdj'}))
            profile = root/'profile'
            profile.mkdir()
            database = profile/'existing.db'
            database.write_bytes(b'private')
            with patch('rb_waveform_core.mac_copy.MAC_COPY_ISOLATION_VERIFIED', True), patch('rb_waveform_core.mac_copy._run'):
                with self.assertRaisesRegex(ValueError, 'existing user data is never reused'):
                    prepare_rekordbox_copy(source, root/'copy.app', profile)
            self.assertEqual(database.read_bytes(), b'private')
            self.assertFalse((root/'copy.app').exists())

    def test_failed_staging_leaves_no_app_or_profile_and_can_retry(self):
        import plistlib
        import shutil
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'vendor.app'
            binary = source/'Contents/MacOS/rekordbox'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'vendor')
            (source/'Contents/Info.plist').write_bytes(plistlib.dumps({
                'CFBundleShortVersionString':'7.2.19.0342', 'CFBundleIdentifier':'com.pioneerdj.rekordboxdj'}))
            copy, profile = root/'copy.app', root/'profile'
            def failed_run(argv):
                if argv[0] == '/usr/bin/ditto':
                    argv[2].mkdir()
                    raise OSError('disk full')
                return SimpleNamespace(stdout=b'')
            with patch('rb_waveform_core.mac_copy.MAC_COPY_ISOLATION_VERIFIED', True), patch('rb_waveform_core.mac_copy._run', side_effect=failed_run):
                with self.assertRaises(OSError):
                    prepare_rekordbox_copy(source, copy, profile)
            self.assertFalse(copy.exists())
            self.assertFalse(profile.exists())
            self.assertFalse(list(root.glob('.rekordbox-wave-*')))
            def successful_run(argv):
                if argv[0] == '/usr/bin/ditto':
                    shutil.copytree(argv[1], argv[2])
                if argv[0] == '/usr/bin/getconf':
                    return SimpleNamespace(stdout=str(root/'darwin-temp').encode())
                return SimpleNamespace(stdout=plistlib.dumps({}) if '-d' in argv else b'')
            with patch('rb_waveform_core.mac_copy.MAC_COPY_ISOLATION_VERIFIED', True), patch('rb_waveform_core.mac_copy._run', side_effect=successful_run):
                launch = prepare_rekordbox_copy(source, copy, profile)
            self.assertEqual(launch.executable, (copy/'Contents/MacOS/rekordbox').resolve())
            self.assertEqual(binary.read_bytes(), b'vendor')
            self.assertEqual(launch.environment['HOME'], launch.environment['CFFIXED_USER_HOME'])
            self.assertTrue((profile/'copy-manifest.json').exists())

    def test_setup_and_launch_are_disabled_before_any_external_action(self):
        from rb_waveform_core.mac_copy import MacCopyLaunch
        with patch('rb_waveform_core.mac_copy.subprocess.run') as run, patch('rb_waveform_core.mac_copy.subprocess.Popen') as popen:
            with self.assertRaisesRegex(RuntimeError, 'helpers can escape'):
                prepare_rekordbox_copy('/Applications/rekordbox.app', '/tmp/copy.app', '/tmp/profile')
            with self.assertRaisesRegex(RuntimeError, 'helpers can escape'):
                MacCopyLaunch(Path('/tmp/copy.app'), Path('/tmp/profile'), (), {}, Path('/tmp/profile/log')).start()
            run.assert_not_called()
            popen.assert_not_called()
