"""Make the C++ tests' reference data from the Python app, so the port is checked against what it replaces.

Run from the repository root: python app/tests/make_fixtures.py
Writes app/tests/data: a synthetic track's ANLZ files, what the Python app reads from them (expected.json),
and the waveform tiles the Python renderer draws for it (tile-*.png).
"""
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
OUT = ROOT / 'app' / 'tests' / 'data'


def tag(kind, header_rest, body):
    header = kind + struct.pack('>II', 12 + len(header_rest), 12 + len(header_rest) + len(body)) + header_rest
    return header + body


def anlz(tags):
    body = b''.join(tags)
    header = b'PMAI' + struct.pack('>II', 28, 28 + len(body)) + bytes(16)
    return header + body


def ppth(path):
    raw = path.encode('utf-16-be') + b'\0\0'
    return tag(b'PPTH', struct.pack('>I', len(raw)), raw)


def pqtz(beats):
    body = b''.join(struct.pack('>HHI', beat, tempo, ms) for beat, tempo, ms in beats)
    return tag(b'PQTZ', struct.pack('>III', 0, 0x80000, len(beats)), body)


def pwv7(columns):
    body = bytes(v for column in columns for v in column)
    return tag(b'PWV7', struct.pack('>III', 3, len(columns), 0x00960000), body)


def pwvc(gains):
    return tag(b'PWVC', struct.pack('>H', 0), struct.pack('>HHH', *gains))


def pcpt(hot, time_ms, loop_ms=None):
    entry = (b'PCPT' + struct.pack('>III', 0x1c, 0x38, hot) + struct.pack('>II', 4, 0x10000) +
             struct.pack('>HHBBH', 0xffff, 0xffff, 2 if loop_ms else 1, 0, 1000) +
             struct.pack('>II', time_ms, loop_ms if loop_ms else 0xffffffff) + bytes(16))
    return entry


def pcob(hot, entries):
    return tag(b'PCOB', struct.pack('>IHHi', 1 if hot else 0, 0, len(entries), -1), b''.join(entries))


def pcp2(hot, time_ms, rgb, comment='', loop_ms=None):
    text = (comment.encode('utf-16-be') + b'\0\0') if comment else b''
    length = 48 + len(text) + 4
    body = (b'PCP2' + struct.pack('>III', 0x10, length, hot) + struct.pack('>B3x', 2 if loop_ms else 1) +
            struct.pack('>II', time_ms, loop_ms if loop_ms else 0xffffffff) + struct.pack('>B7x', 1) +
            struct.pack('>HHI', 0, 0, len(text)) + text + struct.pack('>BBBB', 1, *rgb))
    return body + bytes(length - len(body))


def pco2(entries):
    return tag(b'PCO2', struct.pack('>IHH', 1, len(entries), 0), b''.join(entries))


def columns(n):
    out = []
    for i in range(n):
        beat = abs(math.sin(i * math.pi / 37)) ** 3
        out.append((int(20 + 75 * beat), int(10 + 60 * abs(math.sin(i / 11))), int(3 + 25 * abs(math.cos(i / 5)))))
    return out


def main():
    track = OUT / 'track'
    track.mkdir(parents=True, exist_ok=True)
    cols = columns(2600)
    beats = [((i % 4) + 1, 12800, 120 + int(i * 468.75)) for i in range(40)]
    (track / 'ANLZ0000.DAT').write_bytes(anlz([ppth('/Music/Wave/Test Track.mp3'), pqtz(beats),
                                               pcob(True, [pcpt(1, 1500), pcpt(3, 9000, 11000)]),
                                               pcob(False, [pcpt(0, 4200)])]))
    (track / 'ANLZ0000.EXT').write_bytes(anlz([ppth('/Music/Wave/Test Track.mp3'),
                                               pco2([pcp2(1, 1500, (12, 200, 99), 'Drop')])]))
    (track / 'ANLZ0000.2EX').write_bytes(anlz([ppth('/Music/Wave/Test Track.mp3'), pwv7(cols), pwvc((120, 80, 50))]))

    from rb_waveform_core.deck_controller import DeckController
    from rb_waveform_core.cues import _cues_from_anlz
    from rb_waveform_core.config import WaveformColorConfig, WaveformRenderConfig
    from rb_waveform_core.qt_app import STYLES
    from rb_waveform_core.qt_display import WaveformTiles
    deck = DeckController()
    deck.load_anlz(track)
    a = deck.analysis
    expected = {
        'columns': len(a.low), 'duration': a.duration_seconds, 'song': str(deck.song_name),
        'low': [float(v) for v in a.low[:50]], 'mid': [float(v) for v in a.mid[:50]], 'high': [float(v) for v in a.high[:50]],
        'grid': [[e.time_ms, e.beat_number, e.bpm] for e in deck.beat_grid_full],
        'bpm': deck.original_bpm,
        'cues': [[c.time_sec, list(c.color), c.is_memory, c.number, c.end_sec] for c in _cues_from_anlz(track)],
    }
    (OUT / 'expected.json').write_text(json.dumps(expected, indent=1))
    colors = WaveformColorConfig(**STYLES['Rekordbox'])
    render = WaveformRenderConfig(low_gain=1.1860465116279069, mid_gain=1.1860465116279069, high_gain=1.3255813953488373)
    for tile, reduction in ((0, 1), (2, 1), (1, 4)):
        WaveformTiles.render_tile(a, tile, colors, render, reduction).save(str(OUT / f'tile-{tile}-{reduction}.png'))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
