import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from rb_waveform_core import updater

NAME = 'Rekordbox-Wave-Windows-x64.exe'
URL = f'https://github.com/{updater.REPO}/releases/download/v9.0.0/{NAME}'
DATA = b'new wave' * 1000


class Answer(io.BytesIO):
    def __init__(self, data, url=URL):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url


def release(tag='v9.0.0', digest='sha256:' + hashlib.sha256(DATA).hexdigest(), url=URL, **extra):
    asset = {'name': NAME, 'browser_download_url': url, 'size': len(DATA), 'digest': digest}
    body = json.dumps({'tag_name': tag, 'html_url': 'https://github.com/x', 'assets': [asset], **extra}).encode()
    return lambda _url: Answer(body)


class UpdaterTests(unittest.TestCase):
    def test_versions_compare_as_numbers(self):
        self.assertEqual(updater.version_tuple('v2.3.10'), (2, 3, 10))
        self.assertGreater(updater.version_tuple('v2.3.10'), updater.version_tuple('2.3.9'))
        self.assertIsNone(updater.version_tuple('v2.4.0-beta'))

    def test_each_computer_gets_its_own_download(self):
        self.assertEqual(updater.asset_name('win32'), NAME)
        self.assertEqual(updater.asset_name('darwin', 'arm64'), 'Rekordbox-Wave-macOS-AppleSilicon.dmg')
        self.assertEqual(updater.asset_name('darwin', 'x86_64'), 'Rekordbox-Wave-macOS-Intel.dmg')

    def test_only_a_newer_checksummed_github_release_is_offered(self):
        update = updater.check('2.3.3', release(), NAME)
        self.assertEqual((update.version, update.url, update.size), ('9.0.0', URL, len(DATA)))
        self.assertIsNone(updater.check('9.0.0', release(), NAME))                       # same version
        self.assertIsNone(updater.check('2.3.3', release(prerelease=True), NAME))
        self.assertIsNone(updater.check('2.3.3', release(digest=''), NAME))              # no checksum
        self.assertIsNone(updater.check('2.3.3', release(url='https://evil.example/' + NAME), NAME))
        self.assertIsNone(updater.check('2.3.3', release(), 'Rekordbox-Wave-macOS-Intel.dmg'))  # not in it

    def test_download_is_kept_only_when_the_checksum_matches(self):
        update = updater.check('2.3.3', release(), NAME)
        with tempfile.TemporaryDirectory() as folder:
            seen = []
            path = updater.download(update, folder, lambda done, total: seen.append(done), lambda _url: Answer(DATA))
            self.assertEqual(path.read_bytes(), DATA)
            self.assertEqual(seen[-1], len(DATA))
            path.unlink()
            with self.assertRaises(OSError):
                updater.download(update, folder, fetch=lambda _url: Answer(DATA[:-1] + b'x'))
            with self.assertRaises(OSError):
                updater.download(update, folder, fetch=lambda _url: Answer(DATA, 'https://evil.example/x'))
            self.assertEqual(list(Path(folder).iterdir()), [])

    @unittest.skipUnless(sys.platform == 'win32', 'Windows swap')
    def test_windows_swaps_the_running_exe_and_starts_the_new_one(self):
        with tempfile.TemporaryDirectory() as folder:
            exe, new = Path(folder) / 'Rekordbox Wave.exe', Path(folder) / 'download.exe'
            exe.write_bytes(b'old')
            new.write_bytes(b'new')
            with patch.object(sys, 'executable', str(exe)), patch('subprocess.Popen') as popen:
                updater.install(new)
            self.assertEqual(exe.read_bytes(), b'new')
            self.assertEqual((Path(folder) / 'Rekordbox Wave.old.exe').read_bytes(), b'old')
            self.assertEqual(popen.call_args.args[0], [str(exe)])
            self.assertFalse(any(k.startswith('_MEIPASS') for k in popen.call_args.kwargs['env']))


if __name__ == '__main__':
    unittest.main()
