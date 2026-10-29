import hashlib
import tempfile
import unittest
from pathlib import Path
from sqlcipher3 import dbapi2
from rb_waveform_core.library import ReadOnlyLibrary


class ReadOnlyLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/'master.db'
        con = dbapi2.connect(str(self.path))
        con.execute("PRAGMA key='test-key'")
        con.executescript('''
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,Length INT,BPM INT,FolderPath TEXT,AnalysisDataPath TEXT);
          CREATE TABLE djmdCue(ContentID TEXT,InMsec INT,OutMsec INT,Kind INT);
          INSERT INTO djmdArtist VALUES('artist','Example');
          INSERT INTO djmdContent VALUES('1','Song','artist',180,12800,'/music/song.wav','/PIONEER/USBANLZ/first/ANLZ.DAT');
          INSERT INTO djmdCue VALUES('1',1000,3000,1);
          INSERT INTO djmdContent VALUES('2','Duplicate','artist',180,12800,'/a.wav','/a/ANLZ.DAT');
          INSERT INTO djmdContent VALUES('3','Duplicate','artist',200,12800,'/b.wav','/b/ANLZ.DAT');
        ''')
        con.commit(); con.close()
        self.before = hashlib.sha256(self.path.read_bytes()).digest()
        self.db = ReadOnlyLibrary(self.path,key='test-key')

    def tearDown(self):
        self.db.close()
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).digest(),self.before)
        self.temp.cleanup()

    def test_title_resolves_to_analysis_and_cues_without_writes(self):
        track = self.db.resolve('Song','Example',180,128)
        self.assertEqual(track.identifier,'1')
        self.assertEqual(track.analysis_folder,self.path.parent/'share/PIONEER/USBANLZ/first')
        self.assertEqual(self.db.cues(track.analysis_folder),[(1000,3000,1)])
        self.assertIsNone(self.db.resolve("Song' OR 1=1 --"))

    def test_write_is_rejected_even_if_query_only_is_disabled(self):
        self.db.connection.execute('PRAGMA query_only=OFF')
        with self.assertRaises(dbapi2.OperationalError):
            self.db.connection.execute("DELETE FROM djmdContent")

    def test_incorrect_artist_or_duration_does_not_select_another_track(self):
        self.assertIsNone(self.db.resolve('Song','Other'))
        self.assertIsNone(self.db.resolve('Song','Example',200))
        self.assertIsNone(self.db.resolve('Song','Example',180,130))

    def test_duplicate_titles_require_disambiguating_metadata(self):
        self.assertIsNone(self.db.resolve('Duplicate'))
        self.assertEqual(self.db.resolve('Duplicate','Example',180).identifier,'2')

    def test_cue_reader_uses_read_only_library(self):
        from unittest.mock import patch
        from rb_waveform_core.cues import _cues_from_db
        with patch('rb_waveform_core.cues._get_db',return_value=self.db):
            cues = _cues_from_db(Path('/first'))
        self.assertEqual(len(cues),1)
        self.assertEqual((cues[0].time_sec,cues[0].end_sec,cues[0].number),(1,3,1))
        self.assertFalse(cues[0].is_memory)

    def test_hot_cue_kinds_skip_four(self):
        # master.db stores hot cues A-H as Kind 1, 2, 3, 5, 6, 7, 8, 9.
        from unittest.mock import Mock, patch
        from rb_waveform_core.cues import _cues_from_db
        db = Mock(cues=Mock(return_value=[(1000, -1, kind) for kind in (0, 1, 2, 3, 5, 6, 7, 8, 9)]))
        with patch('rb_waveform_core.cues._get_db', return_value=db):
            cues = _cues_from_db(Path('/first'))
        self.assertEqual([c.number for c in cues], [0, 1, 2, 3, 4, 5, 6, 7, 8])
        self.assertTrue(cues[0].is_memory)

