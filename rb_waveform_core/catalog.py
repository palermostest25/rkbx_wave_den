"""Paged, read-only browsing of the Rekordbox collection and USB exports, the way a deck browses:
by playlist, artist, album, key or history, or every track. Connections belong to the caller's worker."""
from dataclasses import dataclass
from pathlib import Path
import shutil

from .library import ReadOnlyLibrary, DeviceLibrary, device_library_paths


@dataclass(frozen=True)
class CatalogTrack:
    identifier: str
    title: str
    artist: str
    bpm: float
    key: str
    duration: float
    path: str
    artwork: str = ''   # absolute image path, '' for none


@dataclass(frozen=True)
class Category:
    identifier: str
    name: str
    count: int          # tracks inside (0 for a folder)
    folder: bool = False


SORTS = ('Title', 'Artist', 'BPM', 'Key', 'Time')
# Rekordbox keeps streaming metadata encrypted ($A7:v1:...); those entries are left out, never shown raw.
ENCRYPTED = "'$A%:v%:%'"
ORDER = 'Order'         # a playlist's or history's own running order
KINDS = ('PLAYLIST', 'ARTIST', 'ALBUM', 'KEY', 'HISTORY')


class Schema:
    """One library's tables, as the collection (master.db) or a stick (exportLibrary.db) names them."""

    def __init__(self, library, device):
        self.library = library
        self.device = device
        connection = library.connection
        self.tables = {r[0] for r in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        self.columns = {t: {r[1] for r in connection.execute(f'PRAGMA table_info("{t}")')} for t in self.tables}

    def has(self, table, *columns):
        return table in self.tables and set(columns) <= self.columns[table]

    def track_query(self):
        """(fields, tables, condition) for a track listing: id, title, artist, bpm, key, seconds, path, image."""
        if self.device:
            key = 'k.name' if self.has('key', 'key_id', 'name') and self.has('content', 'key_id') else "''"
            image = 'i.path' if self.has('image', 'image_id', 'path') and self.has('content', 'image_id') else "''"
            fields = (f"c.content_id,c.title,coalesce(a.name,''),coalesce(c.bpmx100,0)/100.0,coalesce({key},''),"
                      f"coalesce(c.length,0)/1000.0,c.path,coalesce({image},'')")
            tables = 'content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist'
            if key != "''":
                tables += ' LEFT JOIN "key" k ON k.key_id=c.key_id'
            if image != "''":
                tables += ' LEFT JOIN image i ON i.image_id=c.image_id'
            return fields, tables, '1=1'
        image = 'c.ImagePath' if self.has('djmdContent', 'ImagePath') else "''"
        fields = (f"c.ID,c.Title,coalesce(a.Name,''),coalesce(c.BPM,0)/100.0,coalesce(k.ScaleName,''),"
                  f"coalesce(c.Length,0),c.FolderPath,coalesce({image},'')")
        tables = 'djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID LEFT JOIN djmdKey k ON k.ID=c.KeyID'
        return fields, tables, f'coalesce(c.rb_local_deleted,0)=0 AND c.Title NOT LIKE {ENCRYPTED}'

    def sort_fields(self):
        if self.device:
            return dict(Title='c.title', Artist='a.name', BPM='c.bpmx100',
                        Key='k.name' if self.has('key', 'name') and self.has('content', 'key_id') else 'c.title',
                        Time='c.length'), 'c.content_id'
        return dict(Title='c.Title', Artist='a.Name', BPM='c.BPM', Key='k.ScaleName', Time='c.Length'), 'c.ID'

    def tree(self, kind):
        """(table, id, name, parent, order, attribute) for a playlist or history tree, or None."""
        if self.device:
            table = 'playlist' if kind == 'PLAYLIST' else 'history'
            spec = (table, f'{table}_id', 'name', f'{table}_id_parent', 'sequenceNo', 'attribute')
        else:
            table = 'djmdPlaylist' if kind == 'PLAYLIST' else 'djmdHistory'
            spec = (table, 'ID', 'Name', 'ParentID', 'Seq', 'Attribute')
        return spec if self.has(table, *spec[1:]) else None

    def members(self, kind):
        """(table, list id column, content column, order column) linking a list to its tracks."""
        if self.device:
            spec = ('playlist_content', 'playlist_id', 'content_id', 'sequenceNo') if kind == 'PLAYLIST' else \
                   ('history_content', 'history_id', 'content_id', 'sequenceNo')
        else:
            spec = ('djmdSongPlaylist', 'PlaylistID', 'ContentID', 'TrackNo') if kind == 'PLAYLIST' else \
                   ('djmdSongHistory', 'HistoryID', 'ContentID', 'TrackNo')
        return spec if self.has(*spec) else None

    def flat(self, kind):
        """(table, id, name, track column) for ARTIST, ALBUM or KEY, or None."""
        if self.device:
            spec = dict(ARTIST=('artist', 'artist_id', 'name', 'artist_id_artist'),
                        ALBUM=('album', 'album_id', 'name', 'album_id'),
                        KEY=('key', 'key_id', 'name', 'key_id'))[kind]
            return spec if self.has(*spec[:3]) and self.has('content', spec[3]) else None
        spec = dict(ARTIST=('djmdArtist', 'ID', 'Name', 'ArtistID'), ALBUM=('djmdAlbum', 'ID', 'Name', 'AlbumID'),
                    KEY=('djmdKey', 'ID', 'ScaleName', 'KeyID'))[kind]
        return spec if self.has(*spec[:3]) and self.has('djmdContent', spec[3]) else None

    @property
    def artwork_root(self):
        # The collection's images live under its share folder; a stick's under its own root.
        return self.library.path.parents[2] if self.device else self.library.path.parent/'share'


def _open(source):
    library = DeviceLibrary(Path(source)) if source else ReadOnlyLibrary()
    return library, Schema(library, bool(source))


def sources():
    return [('Collection', '')] + [(p.parents[2].name, str(p)) for p in device_library_paths()]


def source_stats(source=''):
    """What the SOURCE screen shows: tracks, playlists and, for a stick, its space."""
    library, schema = _open(source)
    try:
        fields, tables, condition = schema.track_query()
        tracks = library.connection.execute(f'SELECT count(*) FROM {tables} WHERE {condition}').fetchone()[0]
        tree = schema.tree('PLAYLIST')
        playlists = library.connection.execute(
            f'SELECT count(*) FROM {tree[0]} WHERE coalesce({tree[5]},0)=0').fetchone()[0] if tree else 0
        stats = dict(tracks=tracks, playlists=playlists)
        if source:
            usage = shutil.disk_usage(Path(source).parents[2])
            stats.update(total=usage.total, free=usage.free)
        return stats
    finally:
        library.close()


def categories(source, kind, parent=None):
    """The lists under one browse category: playlists and history (folders drill down), or every
    artist, album or key with tracks. Smart playlists are left out: their rules live in Rekordbox."""
    if kind not in KINDS:
        raise ValueError('Unknown browse category')
    library, schema = _open(source)
    try:
        connection = library.connection
        if kind in ('PLAYLIST', 'HISTORY'):
            tree, members = schema.tree(kind), schema.members(kind)
            if not tree or not members:
                return []
            table, ident, name, parent_col, order, attribute = tree
            root = ("coalesce({0},'root') IN ('root','0','')" if not schema.device else 'coalesce({0},0)=0').format(parent_col)
            where = root if parent is None else f'{parent_col}=?'
            rows = connection.execute(
                f'SELECT p.{ident},p.{name},coalesce(p.{attribute},0),'
                f'(SELECT count(*) FROM {members[0]} m WHERE m.{members[1]}=p.{ident}) '
                f'FROM {table} p WHERE {where} AND coalesce(p.{attribute},0) IN (0,1) ORDER BY p.{order}',
                () if parent is None else (parent,)).fetchall()
            return [Category(str(i), n or 'Untitled', int(c), int(a) == 1) for i, n, a, c in rows]
        flat = schema.flat(kind)
        if not flat:
            return []
        table, ident, name, column = flat
        fields, tables, condition = schema.track_query()
        rows = connection.execute(
            f'SELECT x.{ident},x.{name},count(*) FROM {tables} JOIN {table} x ON x.{ident}=c.{column} '
            f"WHERE {condition} AND trim(coalesce(x.{name},''))<>'' AND x.{name} NOT LIKE {ENCRYPTED} "
            f'GROUP BY x.{ident} ORDER BY x.{name} COLLATE NOCASE').fetchall()
        return [Category(str(i), n, int(c)) for i, n, c in rows]
    finally:
        library.close()


def browse(source='', search='', sort='Title', descending=False, offset=0, limit=200, scope=None):
    """One page of tracks, optionally within ``scope`` = (kind, list id). Returns (total, tracks)."""
    if (sort not in SORTS and not (sort == ORDER and scope and scope[0] in ('PLAYLIST', 'HISTORY'))) \
            or not 0 <= offset <= 10_000_000 or not 1 <= limit <= 500:
        raise ValueError('Invalid browser page or sort')
    if scope is not None and scope[0] not in KINDS:
        raise ValueError('Unknown browse category')
    library, schema = _open(source)
    try:
        fields, tables, condition = schema.track_query()
        sorts, identity = schema.sort_fields()
        title, artist = sorts['Title'], sorts['Artist']
        parameters = []
        order = None
        if scope:
            kind, identifier = scope
            if kind in ('PLAYLIST', 'HISTORY'):
                members = schema.members(kind)
                if not members:
                    return 0, []
                table, list_col, content_col, order_col = members
                tables += f' JOIN {table} m ON m.{content_col}={identity}'
                condition += f' AND m.{list_col}=?'
                order = f'm.{order_col}'
            else:
                flat = schema.flat(kind)
                if not flat:
                    return 0, []
                condition += f' AND c.{flat[3]}=?'
            parameters.append(identifier)
        for word in search.split():
            # Literal substring search: % and _ typed on the touch keyboard are not wildcards.
            pattern = '%' + word.replace('\\', '\\\\').replace('%', '\\%').replace('_', '\\_') + '%'
            condition += f" AND ({title} LIKE ? ESCAPE '\\' OR {artist} LIKE ? ESCAPE '\\')"
            parameters.extend((pattern, pattern))
        count = library.connection.execute(f'SELECT count(*) FROM {tables} WHERE {condition}', parameters).fetchone()[0]
        direction = 'DESC' if descending else 'ASC'
        # Only whitelisted identifiers enter SQL; all search text stays bound.
        field = order if sort == ORDER else f'{sorts[sort]} COLLATE NOCASE'
        rows = library.connection.execute(f'SELECT {fields} FROM {tables} WHERE {condition} '
            f'ORDER BY {field} {direction}, {title} COLLATE NOCASE, {identity} LIMIT ? OFFSET ?',
            parameters + [limit, offset]).fetchall()
        root = schema.artwork_root
        return count, [CatalogTrack(str(i), t or '', a, float(b), k, float(d), p or '',
                                    str(root/str(img).replace('\\', '/').lstrip('/')) if img else '')
                       for i, t, a, b, k, d, p, img in rows]
    finally:
        library.close()
