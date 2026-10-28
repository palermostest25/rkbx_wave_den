# Copyright (c) mrmilbe

"""Hot-cue / memory-cue extraction for a loaded track.

Where cues live depends on the source:
  - Local library tracks: the ANLZ files under the rekordbox AppData ``share``
    folder do NOT contain cues (Rekordbox keeps them in its encrypted master.db
    and only writes them into ANLZ on export). So for local tracks we read them
    from master.db via pyrekordbox, mapping the ANLZ folder -> ContentID.
  - Exported Pioneer USB sticks (audio under ``X:\\Contents``): the ANLZ files on
    the stick are populated on export, so we read the cues straight from them.

Both hot cues and memory cues are returned.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List

from pyrekordbox.anlz import AnlzFile



# Fixed hot-cue colours by slot (A=1 .. H=8).
_HOTCUE_SLOT_COLORS = {
    1: (230, 40, 40),     # A red
    2: (50, 200, 215),    # B light blue / turquoise
    3: (45, 125, 45),     # C dark grass green
    4: (160, 60, 205),    # D purple
    5: (70, 230, 70),     # E bright green
    6: (255, 145, 20),    # F orange
    7: (35, 70, 205),     # G dark blue
    8: (240, 220, 45),    # H yellow
}
_DEFAULT_HOTCUE_RGB = (0, 200, 130)    # fallback when slot is unknown
_DEFAULT_MEMORY_RGB = (255, 190, 0)    # amber, for memory cues


@dataclass(frozen=True)
class CueMarker:
    """A single cue point to draw on the waveform."""
    time_sec: float
    color: tuple          # (r, g, b)
    is_memory: bool        # True = memory cue, False = hot cue
    number: int = 0        # hot-cue slot (1-8) when applicable
    end_sec: float | None = None


# Local rekordbox analysis root (cues are NOT stored in these ANLZ files).
_LOCAL_SHARE = ((Path.home() / "Library" / "Pioneer" / "rekordbox" / "share")
                if sys.platform == "darwin" else
                Path(os.environ.get("APPDATA", str(Path.home()))) / "Pioneer" / "rekordbox" / "share")

_DB = None
_DB_FAILED = False


def _marker_color(is_memory: bool, number: int) -> tuple:
    """Memory cues are amber; hot cues are coloured by their slot (A-H)."""
    if is_memory:
        return _DEFAULT_MEMORY_RGB
    return _HOTCUE_SLOT_COLORS.get(number, _DEFAULT_HOTCUE_RGB)


def _is_local(folder: Path) -> bool:
    try:
        return folder.resolve().is_relative_to(_LOCAL_SHARE.resolve())
    except (ValueError, OSError):
        return False


def get_track_cues(folder: Path) -> List[CueMarker]:
    """Return hot + memory cues for a track's ANLZ folder (local DB or stick)."""
    folder = Path(folder)
    try:
        if _is_local(folder):
            return _cues_from_db(folder)
        return _cues_from_anlz(folder)
    except Exception as e:
        print(f"[Cues] Failed to load cues for {folder}: {e}")
        return []


# ---------------------------------------------------------------------------
# Local library: Rekordbox master.db via pyrekordbox
# ---------------------------------------------------------------------------

def _get_db():
    global _DB, _DB_FAILED
    if _DB is None and not _DB_FAILED:
        try:
            try:
                from pyrekordbox import MasterDatabase
            except ImportError:
                from pyrekordbox import Rekordbox6Database as MasterDatabase
            _DB = MasterDatabase()
        except Exception as e:
            print(f"[Cues] Could not open Rekordbox database: {e}")
            _DB_FAILED = True
    return _DB


def _cues_from_db(folder: Path) -> List[CueMarker]:
    db = _get_db()
    if db is None:
        return []
    try:
        from pyrekordbox.masterdb.models import DjmdContent
    except ImportError:
        from pyrekordbox.db6.tables import DjmdContent
    db.session.expire_all()

    # ANLZ folder names are unique content GUIDs; match the AnalysisDataPath.
    content = (
        db.session.query(DjmdContent)
        .filter(DjmdContent.AnalysisDataPath.like(f"%/{folder.name}/%"))
        .first()
    )
    if content is None:
        return []

    out: List[CueMarker] = []
    for cue in db.get_cue(ContentID=content.ID):
        in_ms = getattr(cue, "InMsec", None)
        if in_ms is None:
            continue
        is_mem = bool(getattr(cue, "is_memory_cue", False))
        number = int(getattr(cue, "Kind", 0) or 0)
        out.append(CueMarker(
            time_sec=float(in_ms) / 1000.0,
            color=_marker_color(is_mem, number),
            is_memory=is_mem,
            number=number,
            end_sec=_loop_end(in_ms, getattr(cue, "OutMsec", None)),
        ))
    return out


# ---------------------------------------------------------------------------
# Exported USB stick: cues live in the (populated) ANLZ files
# ---------------------------------------------------------------------------

def _loop_end(start_ms, end_ms):
    try:
        value = float(end_ms)
        if float(start_ms) < value < 0xffffffff:
            return value / 1000.0
    except (TypeError, ValueError):
        pass
    return None


def _cues_from_anlz(folder: Path) -> List[CueMarker]:
    # PCO2 (usually EXT) carries colours and comments missing from PCOB (DAT).
    # Scan every file; richer entries replace the legacy record for the same slot.
    found = {}
    for suffix in ("DAT", "EXT", "2EX"):
        for path in sorted(folder.glob(f"*.{suffix}")):
            try:
                tags = AnlzFile.parse_file(str(path)).tags or []
            except Exception as exc:
                print(f"[Cues] Cannot parse {path}: {exc}")
                continue
            for tag in tags:
                name = type(tag).__name__
                if not name.startswith(("PCOB", "PCO2")):
                    continue
                content = tag.struct.content
                for e in content.entries or []:
                    t = getattr(e, "time", None)
                    if t is None or t == 0xffffffff:
                        continue
                    number = int(getattr(e, "hot_cue", 0) or 0)
                    is_mem = number == 0
                    key = (int(t), is_mem, number)
                    rich = name.startswith("PCO2")
                    if key in found and found[key][0] and not rich:
                        continue
                    rgb = tuple(int(getattr(e, "color_" + c, 0)) for c in ("red", "green", "blue"))
                    color = rgb if rich and any(rgb) else _marker_color(is_mem, number)
                    is_loop = str(getattr(e, "type", "")) in ("loop", "2")
                    found[key] = (rich, CueMarker(
                        time_sec=float(t) / 1000.0, color=color,
                        is_memory=is_mem, number=number,
                        end_sec=_loop_end(t, getattr(e, "loop_time", None)) if is_loop else None))
    return sorted((item[1] for item in found.values()), key=lambda cue: (cue.time_sec, cue.number))
