"""Strictly read-only access to Rekordbox track metadata and cue points."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


def _database_key():
    from pyrekordbox.masterdb.database import BLOB
    from pyrekordbox.utils import deobfuscate
    return deobfuscate(BLOB)


def _database_path():
    from pyrekordbox.config import get_config
    for version in ('rekordbox7', 'rekordbox6'):
        path = (get_config(version) or {}).get('db_path')
        if path:
            return Path(path)
    raise FileNotFoundError('Rekordbox library database was not found')


def _normal(text):
    return ' '.join(text.casefold().split())


@dataclass(frozen=True)
class LibraryTrack:
    identifier: str
    title: str
    artist: str
    length: int
    bpm: float
    audio_path: Path
    analysis_folder: Path


class ReadOnlyLibrary:
    def __init__(self, path=None, *, key=None):
        from sqlcipher3 import dbapi2
        self.path = Path(path) if path else _database_path()
        # mode=ro is enforced by SQLite, rather than relying on caller discipline.
        self.connection = dbapi2.connect(self.path.resolve().as_uri()+'?mode=ro', uri=True, timeout=.25)
        try:
            key = _database_key() if key is None else key
            self.connection.execute("PRAGMA key = '"+key.replace("'", "''")+"'")
            self.connection.execute('PRAGMA query_only=ON')
            self.connection.execute('SELECT count(*) FROM sqlite_master').fetchone()
        except Exception:
            self.connection.close()
            raise

    def close(self):
        self.connection.close()

    QUERY = '''
        SELECT c.ID,c.Title,coalesce(a.Name,''),c.Length,c.BPM,c.FolderPath,c.AnalysisDataPath,
               (SELECT count(*) FROM djmdCue q WHERE q.ContentID=c.ID),
               coalesce(c.DJPlayCount,0),coalesce(c.updated_at,'')
        FROM djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID
        WHERE c.Title=? COLLATE NOCASE AND coalesce(c.rb_local_deleted,0)=0 LIMIT 100
    '''

    @property
    def analysis_root(self):
        return self.path.parent/'share'

    def resolve(self, title, artist='', length=None, bpm=None):
        rows = self.connection.execute(self.QUERY, (title,)).fetchall()
        tracks = []
        for identifier, name, performer, duration, tempo, audio, analysis, cues, plays, updated in rows:
            if artist and _normal(performer) != _normal(artist):
                continue
            if not analysis:
                continue
            relative = Path(str(analysis).replace('\\','/').lstrip('/'))
            folder = self.analysis_root/relative.parent
            track = LibraryTrack(str(identifier), name, performer, round(float(duration or 0)),
                float(tempo or 0)/100, Path(audio or ''), folder)
            tracks.append((track, (folder.is_dir(), int(cues or 0), int(plays or 0), str(updated))))
        if length is not None:
            # The deck's clocks give the length to a tenth, and a stream decodes up to a couple
            # of seconds off the library's whole-second Length; other versions differ by more.
            tracks = [item for item in tracks if abs(item[0].length - length) <= 2.5]
        if not tracks:
            return None
        def tempo_matches(track):
            # The deck shows the playing BPM, so this only tells versions apart at their own tempo.
            return bpm is not None and track.bpm > 0 and abs(track.bpm - bpm) < .05
        if any(tempo_matches(t) for t, _ in tracks):
            tracks = [item for item in tracks if tempo_matches(item[0])]
        if length is not None:
            nearest = min(abs(t.length - length) for t, _ in tracks)
            tracks = [item for item in tracks if abs(item[0].length - length) <= nearest + 1]
        # Different lengths under one title are different recordings: no basis to choose.
        if max(t.length for t, _ in tracks) - min(t.length for t, _ in tracks) > 1:
            return None
        # Re-imports and streams of one recording share title, artist and length, so any copy
        # draws the right waveform; show the one the DJ prepared: analysed, most cues, most
        # played, latest.
        return max(tracks, key=lambda item: item[1])[0]

    def cues(self, folder):
        return self.connection.execute('''
            SELECT q.InMsec,q.OutMsec,q.Kind FROM djmdCue q
            JOIN djmdContent c ON c.ID=q.ContentID
            WHERE c.AnalysisDataPath LIKE ? AND coalesce(q.rb_local_deleted,0)=0
        ''', ('%/'+Path(folder).name+'/%',)).fetchall()


class DeviceLibrary(ReadOnlyLibrary):
    """A rekordbox USB export (OneLibrary exportLibrary.db): tracks played straight off a stick."""
    QUERY = '''
        SELECT c.content_id,c.title,coalesce(a.name,''),c.length/1000.0,c.bpmx100,c.path,
               c.analysisDataFilePath,(SELECT count(*) FROM cue q WHERE q.content_id=c.content_id),0,''
        FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist
        WHERE c.title=? COLLATE NOCASE LIMIT 100
    '''

    @property
    def analysis_root(self):
        return self.path.parent.parent.parent  # <stick>/PIONEER/rekordbox/exportLibrary.db


def device_library_paths():
    """exportLibrary.db on every mounted drive: /Volumes on macOS, drive letters on Windows."""
    import sys
    if sys.platform == 'win32':
        roots = [Path(f'{letter}:/') for letter in 'DEFGHIJKLMNOPQRSTUVWXYZ']
    else:
        roots = [p for p in Path('/Volumes').glob('*') if not p.is_symlink()]
    found = []
    for root in roots:
        try:
            path = root/'PIONEER'/'rekordbox'/'exportLibrary.db'
            if path.is_file():
                found.append(path)
        except OSError:
            pass
    return found


class Libraries:
    """The collection first, then any rekordbox USB stick, for decks playing from either."""
    def __init__(self, main=None):
        self.main = main
        self.devices = {}  # path -> (mtime, DeviceLibrary)

    def resolve(self, title, artist='', length=None, bpm=None):
        if self.main is None:
            self.main = ReadOnlyLibrary()
        track = self.main.resolve(title, artist, length, bpm)
        if track:
            return track
        paths = device_library_paths()
        for path in list(self.devices):
            if path not in paths:
                self.devices.pop(path)[1].close()
        for path in paths:
            try:
                mtime = path.stat().st_mtime
                if path not in self.devices or self.devices[path][0] != mtime:
                    if path in self.devices:
                        self.devices.pop(path)[1].close()
                    self.devices[path] = (mtime, DeviceLibrary(path))
                track = self.devices[path][1].resolve(title, artist, length, bpm)
            except Exception:
                continue  # an unreadable or older-format stick is skipped, never fatal
            if track:
                return track
        return None

    def close(self):
        if self.main:
            self.main.close()
        for _, library in self.devices.values():
            library.close()
        self.devices.clear()
