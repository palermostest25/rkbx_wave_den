"""Deterministic synthetic tracks for display setup without Rekordbox or audio."""
import numpy as np
from .analysis import WaveformAnalysis
from .ANLZ import BeatGridEntry
from .cues import CueMarker


def populate_demo(decks):
    for idx, deck in decks.items():
        bpm = 128.0
        t = np.arange(180 * 150, dtype=np.float32) / 150
        phase = np.mod(t * bpm / 60, 1)
        rng = np.random.default_rng(42 + idx)
        kick = np.exp(-phase * 11)
        phrasing = .3 + .7 * (np.sin(t / 11 + idx) ** 2)
        low = (.18 + .7 * kick) * phrasing
        mid = (.12 + .5 * np.exp(-np.mod(phase + .5, 1) * 8)) * (.7 + .3 * np.sin(t * 1.7))
        high = (.04 + .28 * np.exp(-np.mod(t * bpm / 60 * 4, 1) * 15)) * rng.uniform(.5, 1, t.size)
        deck.analysis = WaveformAnalysis(low.astype('float32'), mid.astype('float32'), high.astype('float32'), 180.0, 0, 0)
        deck.cached_duration = 180.0
        deck.song_name = f"SYNTHETIC DEMO · Track {idx + 1}"
        deck.original_bpm = deck.current_bpm = bpm
        deck.current_time = 40.0
        deck.beat_grid_full = [BeatGridEntry(n * 60000 / bpm, n % 4 + 1, bpm) for n in range(384)]
        deck.beat_grid_downbeats = deck.beat_grid_full[::4]
        deck.cues = [CueMarker(33.75, (255, 68, 82), False, 1),
                     CueMarker(41.25, (64, 212, 181), False, 2, 45.0),
                     CueMarker(37.5, (255, 190, 54), True)]
