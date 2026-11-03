import sqlite3
import tempfile
import unittest
from concurrent.futures import Future
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QLineEdit
from rb_waveform_core import catalog
from rb_waveform_core.qt_browser import BrowserPage, TouchKeyboard, TrackModel
from rb_waveform_core.qt_app import Session
from rb_waveform_core.rkbx_link_listener import DeckState
from rb_waveform_core.live_state import PlaybackTimeline


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/'library.db'
        c = sqlite3.connect(self.path)
        c.executescript('''
          CREATE TABLE djmdContent(ID TEXT,Title TEXT,ArtistID TEXT,KeyID TEXT,BPM INTEGER,
            Length INTEGER,FolderPath TEXT,rb_local_deleted INTEGER);
          CREATE TABLE djmdArtist(ID TEXT,Name TEXT);
          CREATE TABLE djmdKey(ID TEXT,ScaleName TEXT);
          INSERT INTO djmdArtist VALUES('a','Artist');
          INSERT INTO djmdKey VALUES('k','8A');
          INSERT INTO djmdContent VALUES('1','Ten','a','k',10000,300,'/ten.wav',0);
          INSERT INTO djmdContent VALUES('2','Nine','a','k',9000,90,'/nine.wav',0);
          INSERT INTO djmdContent VALUES('3','100%_remix','a','k',12800,240,'/remix.wav',0);
          INSERT INTO djmdContent VALUES('4','100abc remix','a','k',12900,240,'/other.wav',1);
        ''')
        c.commit(); c.close()
        # Match the application's connection wrapper while enforcing the fixture's read-only URI.
        def open_library():
            connection=sqlite3.connect(self.path.as_uri()+'?mode=ro',uri=True)
            return SimpleNamespace(connection=connection,close=connection.close)
        self.reader = patch.object(catalog,'ReadOnlyLibrary',open_library)
        self.reader.start()

    def tearDown(self):
        self.reader.stop(); self.temp.cleanup()

    def test_numeric_bpm_duration_sort_and_global_paging(self):
        total, rows=catalog.browse(sort='BPM',offset=1,limit=1)
        self.assertEqual(total,3)
        self.assertEqual(rows[0].title,'Ten')
        self.assertEqual(rows[0].bpm,100)
        self.assertEqual(rows[0].key,'8A')
        self.assertEqual(catalog.browse(sort='Time',descending=True,limit=1)[1][0].title,'Ten')

    def test_search_treats_wildcards_and_sql_as_text(self):
        self.assertEqual([r.title for r in catalog.browse(search='100%_ Artist')[1]],['100%_remix'])
        self.assertEqual(catalog.browse(search="' OR 1=1 --")[0],0)
        with self.assertRaises(ValueError): catalog.browse(sort='BPM; DROP TABLE djmdContent')


class TouchAndStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=QApplication.instance() or QApplication([])
        cls.app.setQuitOnLastWindowClosed(False)

    def test_touch_keyboard_preserves_selection_and_physical_text(self):
        field=QLineEdit('track'); keyboard=TouchKeyboard(field)
        field.setSelection(0,5); keyboard.insert('a')
        self.assertEqual(field.text(),'a')
        keyboard.toggle_shift(); keyboard.insert('B')
        self.assertEqual(field.text(),'aB')
        field.backspace(); self.assertEqual(field.text(),'a')
        self.assertGreaterEqual(keyboard.letters[0][0].minimumHeight(),48)

    def test_outdated_query_cannot_replace_new_results(self):
        with patch('rb_waveform_core.qt_browser.sources',return_value=[('Collection','')]):
            page=BrowserPage()
        page.debounce.stop()
        page.running_revision=page.revision
        page.future=Future(); page.future.set_result((1,[]))
        page.request(); page.debounce.stop()
        with patch.object(page,'start_query') as start:
            page.complete_query()
            start.assert_called_once()
        self.assertEqual(page.model.rowCount(),0)
        page.shutdown()

    def test_native_session_ignores_controller_guesses_but_keeps_zoom(self):
        with tempfile.TemporaryDirectory() as directory:
            session=Session(demo=True,config_path=Path(directory)/'settings.json')
            session.demo=False
            timeline=session.timelines[0]
            timeline.infer_loops=False
            timeline.observe(10,observed_at=100)
            for action in ('auto','in','out','hotcue','delete','memory','play'):
                session.loop_action(0,action,1,100)
            self.assertIsNone(timeline.active_loop())
            self.assertIsNone(timeline.pending_in)
            self.assertIsNone(timeline.play_pressed)
            self.assertEqual(session.cue_edits[0],[])
            factors=[]; session.zoomRequested.connect(factors.append)
            session.loop_action(None,'zoom',('relative',1),100)
            self.assertEqual(len(factors),1)
            event=DeckState(0,loop_active=True,loop_start=10,loop_end=12).snapshot('loop')
            session.receive(event)
            self.assertEqual(timeline.active_loop(),(10,12))
            session.close()

    def test_repeated_hotcue_jumps_do_not_confirm_a_loop(self):
        timeline=PlaybackTimeline(infer_loops=False,extrapolate_seconds=.15)
        timeline.beat_seconds=.5
        stamp=0
        for _ in range(4):
            for pos in (10,10.1,10.2,10.3,10.4,10.5,10.6,10.7,10.8,10.9):
                stamp+=.1; timeline.observe(pos,observed_at=stamp)
        self.assertIsNone(timeline.active_loop())
        self.assertIsNone(timeline.inferred_loop)

    def test_search_shortcuts_and_browser_back_navigation(self):
        from PySide6.QtTest import QTest
        from rb_waveform_core.qt_app import MainWindow
        with tempfile.TemporaryDirectory() as directory, \
                patch('rb_waveform_core.qt_browser.sources',return_value=[('Collection','')]), \
                patch('rb_waveform_core.qt_browser.browse',return_value=(0,[])):
            session=Session(demo=True,config_path=Path(directory)/'settings.json')
            window=MainWindow(session); window.show(); window.toggle_browser()
            page=window.browser_page
            page.search.setFocus(); QTest.qWait(30)
            before=session.view['zoom_beats']
            QTest.keyClicks(page.search,'a-b+c=d')
            self.assertEqual(page.search.text(),'a-b+c=d')
            self.assertEqual(session.view['zoom_beats'],before)
            page.keyboard.show(); window.escape()
            self.assertFalse(page.keyboard.isVisible())
            self.assertIs(window.pages.currentWidget(),page)
            window.escape(); self.assertIs(window.pages.currentWidget(),window.canvas)
            window.close(); self.app.processEvents()


class DeviceCatalogTests(unittest.TestCase):
    def test_usb_uses_its_own_key_and_reads_bpm_key_and_millisecond_duration(self):
        from sqlcipher3 import dbapi2
        from pyrekordbox.onelibrary.database import BLOB
        from pyrekordbox.utils import deobfuscate
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'exportLibrary.db'
            connection=dbapi2.connect(str(path))
            key=deobfuscate(BLOB)
            connection.execute("PRAGMA key='"+key.replace("'","''")+"'")
            connection.executescript('''
              CREATE TABLE artist(artist_id INTEGER,name TEXT);
              CREATE TABLE "key"(key_id INTEGER,name TEXT);
              CREATE TABLE content(content_id INTEGER,title TEXT,artist_id_artist INTEGER,bpmx100 INTEGER,
                length INTEGER,path TEXT,key_id INTEGER);
              INSERT INTO artist VALUES(1,'USB Artist');
              INSERT INTO "key" VALUES(1,'8A');
              INSERT INTO content VALUES(1,'USB Song',1,12825,180500,'/Contents/track.wav',1);
            ''')
            connection.commit(); connection.close()
            total,rows=catalog.browse(str(path),sort='BPM')
            self.assertEqual(total,1)
            self.assertEqual((rows[0].bpm,rows[0].key,rows[0].duration),(128.25,'8A',180.5))
