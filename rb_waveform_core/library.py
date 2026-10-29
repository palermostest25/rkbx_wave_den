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

    def resolve(self, title, artist='', length=None, bpm=None):
        rows = self.connection.execute('''
            SELECT c.ID,c.Title,coalesce(a.Name,''),c.Length,c.BPM,c.FolderPath,c.AnalysisDataPath,
                   (SELECT count(*) FROM djmdCue q WHERE q.ContentID=c.ID),
                   coalesce(c.DJPlayCount,0),coalesce(c.updated_at,'')
            FROM djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID
            WHERE c.Title=? COLLATE NOCASE AND coalesce(c.rb_local_deleted,0)=0 LIMIT 100
        ''', (title,)).fetchall()
        tracks = []
        for identifier, name, performer, duration, tempo, audio, analysis, cues, plays, updated in rows:
            if artist and _normal(performer) != _normal(artist):
                continue
            if length is not None and abs(float(duration or 0)-length) > 1.1:
                continue
            if bpm is not None and tempo and abs(float(tempo)/100-bpm) > .1:
                continue
            if not analysis:
                continue
            relative = Path(str(analysis).replace('\\','/').lstrip('/'))
            folder = self.path.parent/'share'/relative.parent
            track = LibraryTrack(str(identifier), name, performer, int(duration or 0),
                float(tempo or 0)/100, Path(audio or ''), folder)
            tracks.append(((folder.is_dir(), int(cues or 0), int(plays or 0), str(updated)), track))
        if not tracks:
            return None
        # Different lengths under one title are different recordings: no basis to choose.
        if max(t.length for _, t in tracks) - min(t.length for _, t in tracks) > 1:
            return None
        # Re-imports of one recording share title, artist and length, so any copy draws the
        # right waveform; show the one the DJ prepared: analysed, most cues, most played, latest.
        return max(tracks, key=lambda item: item[0])[1]

    def cues(self, folder):
        return self.connection.execute('''
            SELECT q.InMsec,q.OutMsec,q.Kind FROM djmdCue q
            JOIN djmdContent c ON c.ID=q.ContentID
            WHERE c.AnalysisDataPath LIKE ? AND coalesce(q.rb_local_deleted,0)=0
        ''', ('%/'+Path(folder).name+'/%',)).fetchall()
