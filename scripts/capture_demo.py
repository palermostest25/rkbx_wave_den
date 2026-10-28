"""Render the real SDL viewer at acceptance sizes. Data is explicitly synthetic."""
import argparse
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pygame
from pygame._sdl2.video import Texture
from rb_waveform_core.rb_waveform_gpu import WaveformGpuWindow, DeckFrame
from rb_waveform_core.deck_controller import DeckController
from rb_waveform_core.demo import populate_demo
from rb_waveform_core.config import WaveformColorConfig, WaveformRenderConfig
from rb_waveform_core.displays import deck_order

parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
for width, height, count in [(1920, 1080, 4), (1280, 720, 4), (1920, 1080, 2)]:
    win = WaveformGpuWindow(count, size=(width, height), row_order=deck_order('3,1,2,4', count), always_on_top=False, borderless=False)
    decks = {i: DeckController() for i in range(count)}
    populate_demo(decks)
    for idx, deck in decks.items():
        image, pps, total = deck.build_full_track_image(WaveformColorConfig(), WaveformRenderConfig(), height=256, pixels_per_second=40, reduce='max')
        win.set_track(idx, image, pps, total, beats=deck.get_beat_markers(), cues=deck.get_cue_markers())
    frames = [DeckFrame(40 + i * .08, 16, d.song_name, d.current_bpm) for i, d in decks.items()]
    win.pump()
    target = Texture(win.renderer, (width, height), target=True)
    win.renderer.target = target
    win.render_frame(frames, present=False, output_size=(width, height))
    pygame.image.save(win.renderer.to_surface(), str(args.out / f'demo-{width}x{height}-{count}deck.png'))
    win.close()
