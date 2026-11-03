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
    key: str = ''
    artwork: Path | None = None


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
               coalesce(c.DJPlayCount,0),coalesce(c.updated_at,''),
               coalesce((SELECT k.ScaleName FROM djmdKey k WHERE k.ID=c.KeyID),''),coalesce(c.ImagePath,'')
        FROM djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID
        WHERE c.Title=? COLLATE NOCASE AND coalesce(c.rb_local_deleted,0)=0 LIMIT 100
    '''
    PATH_QUERY = QUERY.replace('c.Title=? COLLATE NOCASE', 'c.FolderPath=?')

    @property
    def analysis_root(self):
        return self.path.parent/'share'

    def _tracks(self, rows):
        tracks = []
        for identifier, name, performer, duration, tempo, audio, analysis, cues, plays, updated, key, image in rows:
            if not analysis:
                continue
            relative = Path(str(analysis).replace('\\','/').lstrip('/'))
            folder = self.analysis_root/relative.parent
            track = LibraryTrack(str(identifier), name, performer, round(float(duration or 0)),
                float(tempo or 0)/100, Path(audio or ''), folder, key or '',
                self.analysis_root/str(image).replace('\\','/').lstrip('/') if image else None)
            tracks.append((track, (folder.is_dir(), int(cues or 0), int(plays or 0), str(updated))))
        return tracks

    def library_path(self, audio_path):
        """How this library stores ``audio_path``, or None when the file is not in it."""
        return str(audio_path)

    def resolve_path(self, audio_path):
        """The track Rekordbox has loaded from exactly this file."""
        stored = self.library_path(audio_path)
        if not stored:
            return None
        tracks = self._tracks(self.connection.execute(self.PATH_QUERY, (stored,)).fetchall())
        return max(tracks, key=lambda item: item[1])[0] if tracks else None

    def resolve(self, title, artist='', length=None, bpm=None):
        tracks = [item for item in self._tracks(self.connection.execute(self.QUERY, (title,)).fetchall())
                  if not artist or _normal(item[0].artist) == _normal(artist)]
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
    def __init__(self, path=None, *, key=None):
        if key is None:
            from pyrekordbox.onelibrary.database import BLOB
            from pyrekordbox.utils import deobfuscate
            key = deobfuscate(BLOB)
        super().__init__(path, key=key)
        # Older exports have no key table; their tracks show no key rather than failing.
        columns = {r[1] for r in self.connection.execute('PRAGMA table_info(content)')}
        key_columns = {r[1] for r in self.connection.execute('PRAGMA table_info("key")')}
        if 'key_id' in columns and {'key_id', 'name'} <= key_columns:
            keyed = "(SELECT k.name FROM \"key\" k WHERE k.key_id=c.key_id)"
            self.QUERY = self.QUERY.replace("'' /*key*/", f"coalesce({keyed},'')")
            self.PATH_QUERY = self.PATH_QUERY.replace("'' /*key*/", f"coalesce({keyed},'')")

    QUERY = '''
        SELECT c.content_id,c.title,coalesce(a.name,''),c.length/1000.0,c.bpmx100,c.path,
               c.analysisDataFilePath,(SELECT count(*) FROM cue q WHERE q.content_id=c.content_id),0,'','' /*key*/,''
        FROM content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist
        WHERE c.title=? COLLATE NOCASE LIMIT 100
    '''
    PATH_QUERY = QUERY.replace('c.title=? COLLATE NOCASE', 'c.path=?')

    def library_path(self, audio_path):
        # A stick stores paths from its own root: /Volumes/STICK/Contents/x.mp3 is /Contents/x.mp3.
        try:
            return '/' + Path(audio_path).relative_to(self.analysis_root).as_posix()
        except ValueError:
            return None

    @property
    def analysis_root(self):
        return self.path.parent.parent.parent  # <stick>/PIONEER/rekordbox/exportLibrary.db


def device_library_paths():
    """exportLibrary.db on every local drive: USB sticks and disks, never network shares, whose
    servers can take seconds to answer (or never do) and would stall the lookup."""
    import psutil
    found = []
    for partition in psutil.disk_partitions(all=False):
        remote = 'remote' in partition.opts.split(',') or partition.fstype.lower() in NETWORK_FILESYSTEMS
        root = Path(partition.mountpoint)
        if remote or root == Path('/') or str(root).startswith('/System/'):
            continue
        try:
            path = root/'PIONEER'/'rekordbox'/'exportLibrary.db'
            if path.is_file():
                found.append(path)
        except OSError:
            pass
    return found


NETWORK_FILESYSTEMS = {'smbfs', 'nfs', 'afpfs', 'webdav', 'cifs', 'ftp', 'macfuse', 'osxfuse', 'fuse'}


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
        for library in self._devices():
            try:
                track = library.resolve(title, artist, length, bpm)
            except Exception:
                continue  # an unreadable or older-format stick is skipped, never fatal
            if track:
                return track
        return None

    def _devices(self):
        """Open libraries of the sticks mounted now, reopened when a stick's library changes."""
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
            except Exception:
                continue
            yield self.devices[path][1]

    def resolve_path(self, audio_path):
        if self.main is None:
            self.main = ReadOnlyLibrary()
        return self.main.resolve_path(audio_path) or next(
            (track for library in self._devices() if (track := library.resolve_path(audio_path))), None)

    def close(self):
        if self.main:
            self.main.close()
        for _, library in self.devices.values():
            library.close()
        self.devices.clear()
