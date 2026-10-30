import hashlib
import tempfile
import unittest
from pathlib import Path
from sqlcipher3 import dbapi2
from rb_waveform_core.library import ReadOnlyLibrary, DeviceLibrary, Libraries


class ReadOnlyLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/'master.db'
        con = dbapi2.connect(str(self.path))
        con.execute("PRAGMA key='test-key'")
        con.executescript('''
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,Length INT,BPM INT,FolderPath TEXT,AnalysisDataPath TEXT,
                                   DJPlayCount INT,updated_at TEXT,rb_local_deleted INT);
          CREATE TABLE djmdCue(ContentID TEXT,InMsec INT,OutMsec INT,Kind INT,rb_local_deleted INT DEFAULT 0);
          INSERT INTO djmdArtist VALUES('artist','Example');
          INSERT INTO djmdContent VALUES('1','Song','artist',180,12800,'/music/song.wav','/PIONEER/USBANLZ/first/ANLZ.DAT',0,'',0);
          INSERT INTO djmdCue(ContentID,InMsec,OutMsec,Kind) VALUES('1',1000,3000,1);
          INSERT INTO djmdCue VALUES('1',9000,-1,0,1);
          INSERT INTO djmdContent VALUES('2','Duplicate','artist',180,12800,'/a.wav','/a/ANLZ.DAT',0,'',0);
          INSERT INTO djmdContent VALUES('3','Duplicate','artist',200,12800,'/b.wav','/b/ANLZ.DAT',0,'',0);
          INSERT INTO djmdContent VALUES('4','Reimport','artist',197,12200,'/c.m4a','/c/ANLZ.DAT',3,'2026-03-18',0);
          INSERT INTO djmdContent VALUES('5','Reimport','artist',197,12200,'/d.m4a','/d/ANLZ.DAT',0,'2026-10-25',0);
          INSERT INTO djmdContent VALUES('6','Reimport','artist',197,12200,'/e.m4a','/e/ANLZ.DAT',9,'2026-10-29',1);
          INSERT INTO djmdContent VALUES('7','Stream','artist',213,13636,'/v4/catalog/tracks/1/','/s1/ANLZ.DAT',0,'',0);
          INSERT INTO djmdContent VALUES('8','Stream','artist',170,13483,'/v4/catalog/tracks/2/','/s2/ANLZ.DAT',0,'',0);
          INSERT INTO djmdCue(ContentID,InMsec,OutMsec,Kind) VALUES('4',500,-1,1);
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
        # The deck shows the playing BPM: a pitched deck still resolves its only candidate.
        self.assertEqual(self.db.resolve('Song','Example',180,130).identifier, '1')

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

    def test_streaming_versions_sharing_a_title_resolve_by_length_and_bpm(self):
        # Beatport streams drop "(Original Mix)": two versions share one title. A stream's deck
        # length can run over a second past the library's whole-second Length.
        self.assertEqual(self.db.resolve('Stream', 'Example', 171.4).identifier, '8')
        self.assertEqual(self.db.resolve('Stream', 'Example', 214.9, 136.36).identifier, '7')
        self.assertEqual(self.db.resolve('Stream', 'Example', None, 134.83).identifier, '8')
        self.assertIsNone(self.db.resolve('Stream', 'Example'))  # nothing to tell them apart
        self.assertIsNone(self.db.resolve('Stream', 'Example', 190))  # neither version

    def test_track_played_from_a_usb_stick_resolves_from_its_export(self):
        stick = Path(self.temp.name)/'VIAGUS'
        (stick/'PIONEER'/'rekordbox').mkdir(parents=True)
        export = stick/'PIONEER'/'rekordbox'/'exportLibrary.db'
        con = dbapi2.connect(str(export))
        con.execute("PRAGMA key='test-key'")
        con.executescript('''
          CREATE TABLE artist(artist_id INTEGER, name TEXT);
          CREATE TABLE content(content_id INTEGER, title TEXT, artist_id_artist INTEGER, length INTEGER,
                               bpmx100 INTEGER, path TEXT, analysisDataFilePath TEXT);
          CREATE TABLE cue(cue_id INTEGER, content_id INTEGER);
          INSERT INTO artist VALUES(1, 'MPH');
          INSERT INTO content VALUES(10, 'Only On USB', 1, 170960, 13483, '/Contents/MPH/x.mp3',
                                     '/PIONEER/USBANLZ/P0AB/0001C8F3/ANLZ0000.DAT');
        ''')
        con.commit(); con.close()
        device = DeviceLibrary(export, key='test-key')
        track = device.resolve('Only On USB', 'MPH', 171.5, 134.83)
        self.assertEqual(track.analysis_folder, stick/'PIONEER'/'USBANLZ'/'P0AB'/'0001C8F3')
        self.assertEqual(track.length, 171)
        device.close()
        # The collection is asked first; a stick is only searched when the collection misses.
        from unittest.mock import patch
        libraries = Libraries(self.db)
        with patch('rb_waveform_core.library.device_library_paths', return_value=[export]), \
                patch('rb_waveform_core.library.DeviceLibrary', lambda path: DeviceLibrary(path, key='test-key')):
            self.assertEqual(libraries.resolve('Song', 'Example', 180).identifier, '1')
            self.assertEqual(libraries.resolve('Only On USB', 'MPH', 171.5).identifier, '10')
        for _, library in libraries.devices.values():
            library.close()

    def test_reimported_duplicate_resolves_to_the_prepared_copy(self):
        # Same recording imported twice: the copy with cues wins; deleted rows never do.
        self.assertEqual(self.db.resolve('Reimport', 'Example', 197.3).identifier, '4')
        self.assertEqual(self.db.resolve('Reimport', '  example ').identifier, '4')

