"""Paged, read-only collection browsing; connections belong to the caller's worker."""
from dataclasses import dataclass
from pathlib import Path
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


SORTS = ('Title', 'Artist', 'BPM', 'Key', 'Time')


def sources():
    return [('Collection', '')] + [(p.parents[2].name, str(p)) for p in device_library_paths()]


def browse(source='', search='', sort='Title', descending=False, offset=0, limit=200):
    if sort not in SORTS or not 0 <= offset <= 10_000_000 or not 1 <= limit <= 500:
        raise ValueError('Invalid browser page or sort')
    library = DeviceLibrary(Path(source)) if source else ReadOnlyLibrary()
    try:
        if source:
            columns = {r[1] for r in library.connection.execute('PRAGMA table_info(content)')}
            key_columns = {r[1] for r in library.connection.execute('PRAGMA table_info("key")')}
            has_key = 'key_id' in columns and {'key_id', 'name'} <= key_columns
            key = 'k.name' if has_key else "''"
            fields = f"c.content_id,c.title,coalesce(a.name,''),coalesce(c.bpmx100,0)/100.0,coalesce({key},''),coalesce(c.length,0)/1000.0,c.path"
            tables = 'content c LEFT JOIN artist a ON a.artist_id=c.artist_id_artist'
            if has_key:
                tables += ' LEFT JOIN "key" k ON k.key_id=c.key_id'
            title, artist, tempo, duration, identity = 'c.title', 'a.name', 'c.bpmx100', 'c.length', 'c.content_id'
            condition = '1=1'
        else:
            fields = "c.ID,c.Title,coalesce(a.Name,''),coalesce(c.BPM,0)/100.0,coalesce(k.ScaleName,''),coalesce(c.Length,0),c.FolderPath"
            tables = 'djmdContent c LEFT JOIN djmdArtist a ON a.ID=c.ArtistID LEFT JOIN djmdKey k ON k.ID=c.KeyID'
            title, artist, tempo, key, duration, identity = 'c.Title', 'a.Name', 'c.BPM', 'k.ScaleName', 'c.Length', 'c.ID'
            condition = 'coalesce(c.rb_local_deleted,0)=0'
        parameters = []
        for word in search.split():
            # Literal substring search: % and _ typed on the touch keyboard are not wildcards.
            pattern = '%' + word.replace('\\', '\\\\').replace('%', '\\%').replace('_', '\\_') + '%'
            condition += f" AND ({title} LIKE ? ESCAPE '\\' OR {artist} LIKE ? ESCAPE '\\')"
            parameters.extend((pattern, pattern))
        count = library.connection.execute(f'SELECT count(*) FROM {tables} WHERE {condition}', parameters).fetchone()[0]
        field = dict(zip(SORTS, (title, artist, tempo, key, duration)))[sort]
        direction = 'DESC' if descending else 'ASC'
        # Only whitelisted identifiers enter SQL; all search text stays bound.
        rows = library.connection.execute(f'SELECT {fields} FROM {tables} WHERE {condition} '
            f'ORDER BY {field} COLLATE NOCASE {direction}, {title} COLLATE NOCASE, {identity} LIMIT ? OFFSET ?',
            parameters + [limit, offset]).fetchall()
        return count, [CatalogTrack(str(i), t or '', a, float(b), k, float(d), p or '') for i,t,a,b,k,d,p in rows]
    finally:
        library.close()
