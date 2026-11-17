"""Finding, and installing, the Rekordbox Wave supports."""
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from rb_waveform_core import rekordbox_setup as rs
from rb_waveform_core.mac_bridge import windows_executable


class Answer(io.BytesIO):
    def __init__(self, data, url='https://cdn.rekordbox.com/files/1/x.zip'):
        super().__init__(data)
        self.url, self.headers = url, {'Content-Length': str(len(data))}

    def geturl(self):
        return self.url


class InstalledTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def add(self, version):
        folder = self.root / f'rekordbox {version}'
        folder.mkdir()
        (folder / 'rekordbox.exe').write_bytes(b'MZ')

    def test_the_supported_version_is_started_even_beside_a_newer_one(self):
        self.add('7.2.14')
        self.add('7.2.19')
        self.add('7.2.21')
        self.assertEqual(windows_executable(self.root).parent.name, 'rekordbox 7.2.19')
        self.assertIn('also 7.2.14, 7.2.21', rs.summary(self.root, 'win32'))

    def test_without_it_the_newest_is_used_and_the_summary_says_what_to_install(self):
        self.add('7.2.21')
        self.assertEqual(windows_executable(self.root).parent.name, 'rekordbox 7.2.21')
        self.assertIsNone(rs.supported_install(self.root, 'win32'))
        self.assertIn('Install 7.2.19 beside it', rs.summary(self.root, 'win32'))
        self.assertIn('not installed', rs.summary(self.root / 'none', 'win32'))


class DownloadTests(unittest.TestCase):
    def test_a_maintained_official_url_comes_first(self):
        sources = json.dumps({'windows': {'version': rs.SUPPORTED_TEXT, 'url': 'https://cdn.rekordbox.com/files/9/'
                              + rs.WINDOWS_ZIP, 'sha256': 'AB'}}).encode()
        self.assertEqual(rs.find_download('win32', lambda url, timeout=20: Answer(sources)),
                         ('https://cdn.rekordbox.com/files/9/' + rs.WINDOWS_ZIP, 'ab'))

    def test_otherwise_rekordbox_com_is_read_and_anything_else_refused(self):
        page = (f'<a href="https://cdn.rekordbox.com/files/20260924/{rs.WINDOWS_ZIP}">Download</a>').encode()
        bad = json.dumps({'windows': {'version': rs.SUPPORTED_TEXT, 'url': 'https://evil.example/' + rs.WINDOWS_ZIP}}).encode()
        fetch = lambda url, timeout=20: Answer(bad if url == rs.SOURCES else page)
        self.assertEqual(rs.find_download('win32', fetch)[0], f'https://cdn.rekordbox.com/files/20260924/{rs.WINDOWS_ZIP}')
        newer = b'<a href="https://cdn.rekordbox.com/files/1/Install_rekordbox_x64_7_2_21.zip">'
        self.assertEqual(rs.find_download('win32', lambda url, timeout=20: Answer(b'{}' if url == rs.SOURCES else newer)),
                         (None, ''))

    def test_downloads_only_from_rekordbox_com_and_check_the_checksum(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(OSError):
                rs.download('https://evil.example/x.zip', folder)
            with self.assertRaisesRegex(OSError, 'redirected'):
                rs.download('https://cdn.rekordbox.com/x.zip', folder, fetch=lambda u, timeout=60: Answer(b'x', 'https://evil.example/x'))
            with self.assertRaisesRegex(OSError, 'checksum'):
                rs.download('https://cdn.rekordbox.com/x.zip', folder, sha256='00', fetch=lambda u, timeout=60: Answer(b'x'))
            path = rs.download('https://cdn.rekordbox.com/x.zip', folder, fetch=lambda u, timeout=60: Answer(b'zip'))
            self.assertEqual(path.read_bytes(), b'zip')

    def test_the_installer_is_taken_from_the_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / 'a.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Install_rekordbox_x64_7_2_19/readme.txt', 'x')
                z.writestr('Install_rekordbox_x64_7_2_19/Install_rekordbox_x64_7_2_19.exe', b'MZ')
            out = Path(folder) / 'out'
            out.mkdir()
            self.assertEqual(rs.installer_in(archive, out).read_bytes(), b'MZ')

    def test_only_alphatheta_or_pioneer_signatures_are_trusted(self):
        self.assertTrue(rs.trusted('CN=AlphaTheta Corporation, O=AlphaTheta Corporation, L=Yokohama'))
        self.assertTrue(rs.trusted('CN=Pioneer DJ Corporation'))
        self.assertFalse(rs.trusted(''))
        self.assertFalse(rs.trusted('CN=Someone Else'))


if __name__ == '__main__':
    unittest.main()
