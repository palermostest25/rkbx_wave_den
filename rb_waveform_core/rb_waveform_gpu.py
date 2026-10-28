# Copyright (c) mrmilbe

"""GPU-accelerated waveform display in a separate window (pygame / SDL2).

Why this exists:
    The tkinter display path re-resized and re-uploaded the full waveform image to
    a software-rendered Tk PhotoImage every frame (per deck), which dominated CPU.
    Here the full-track waveform is uploaded to the GPU once as a Texture; per-frame
    scrolling/zooming is just a GPU draw of a sub-rectangle into the deck viewport -
    the "move a surface" model, like the OS compositor moving a window.

Position in data flow:
    deck_controller (analysis) -> render.prerender_full_waveform (PIL image, once per
    track/config) -> set_track() uploads it to a GPU Texture -> render_frame() draws
    the visible time window each frame (cheap).

Threading note:
    The Window/Renderer/Textures are created and used from a single thread (the same
    thread that drives render_frame, i.e. the tkinter main thread via root.after). SDL
    rendering must stay on the thread that created the renderer - do not call into this
    from a worker thread.
"""

from __future__ import annotations

import ctypes
import os
import sys
import time
from dataclasses import dataclass
from typing import Optional, Sequence, Tuple

# Linear texture filtering (read by SDL when textures are created / scaled). This
# is what makes the scaled waveform smooth instead of nearest-neighbour "pixely".
os.environ.setdefault("SDL_RENDER_SCALE_QUALITY", "1")

import pygame
from pygame._sdl2.video import Renderer, Texture, Window
from PIL import Image


def place_cue_flag(preferred, width, viewport_width, occupied):
    """Choose a free position on two flag rails, retaining the closest anchor."""
    candidates = []
    for row in (0, 1):
        anchors = [max(0, min(preferred, viewport_width - width)), 0]
        anchors += [x + w + 2 for x, r, w in occupied if r == row]
        for x in anchors:
            if x + width > viewport_width:
                continue
            if all(r != row or x + width + 1 <= ox or x >= ox + ow + 1
                   for ox, r, ow in occupied):
                candidates.append((abs(x - preferred) + row * width / 2, x, row))
    if not candidates:
        return None
    _, x, row = min(candidates)
    occupied.append((x, row, width))
    return x, row


@dataclass
class _DeckTexture:
    texture: Texture
    pixels_per_second: float
    total_duration: float
    downbeats: tuple = ()   # downbeat times in seconds (drawn as overlay lines)
    tiles: tuple = ()
    beats: tuple = ()
    cues: tuple = ()        # (time_sec, (r,g,b), is_memory) cue markers


@dataclass
class DeckFrame:
    """Per-deck state needed to draw one frame."""
    current_time: Optional[float]   # playhead time in seconds, or None if no position yet
    visible_seconds: float          # width of the visible window (zoom x tempo)
    label: str = ""
    bpm: Optional[float] = None


class WaveformGpuWindow:
    """A standalone SDL2 window that draws one scrolling waveform per deck on the GPU."""

    def __init__(
        self,
        deck_count: int,
        size: Tuple[int, int] = (1280, 400),
        *,
        background: Tuple[int, int, int] = (5, 5, 5),
        playhead_color: Tuple[int, int, int] = (255, 0, 0),
        beat_color: Tuple[int, int, int] = (235, 235, 235),
        beat_end_color: Tuple[int, int, int] = (255, 60, 60),
        beat_width: float = 2.0,
        beat_triangles: bool = False,
        vsync: bool = False,
        borderless: bool = True,
        always_on_top: bool = True,
        on_close=None,
        row_order=None,
        ui_scale=1.0,
    ) -> None:
        self.deck_count = max(1, int(deck_count))
        self.row_order = tuple(row_order or range(self.deck_count))
        self.ui_scale = ui_scale
        self._font_size = 0
        self.zoom_delta = 0
        self._bound_display = ""
        self._fill_display = False
        self._display_check = 0.0
        self.display_available = True
        self.background = pygame.Color(*background)
        self.playhead_color = pygame.Color(*playhead_color)
        self.beat_color = pygame.Color(*beat_color)
        self.beat_end_color = pygame.Color(*beat_end_color)
        self._beat_width = float(beat_width)
        self._beat_triangles = bool(beat_triangles)
        self.on_close = on_close
        self._closed = False
        self._title = "rkbx_wave — Waveforms"
        self._hwnd: Optional[int] = None
        self._on_top = bool(always_on_top)

        pygame.init()

        self.window = self._create_window(size, borderless)
        # accelerated=1 forces a hardware renderer; fall back to auto if unavailable.
        try:
            self.renderer = Renderer(self.window, accelerated=1, vsync=vsync)
            self.hardware = True
        except Exception as exc:  # pragma: no cover - depends on host GPU/drivers
            print(f"[GPU] Hardware renderer unavailable ({exc}); using auto renderer")
            self.renderer = Renderer(self.window, accelerated=-1, vsync=vsync)
            self.hardware = False

        # Conservative max texture width; long tracks reduce pixels/second to fit.
        self.max_texture_width = 4096
        self.max_track_width = 32768

        try:
            pygame.font.init()
            self._font = pygame.font.SysFont("Arial", 14)
            self._cue_font = pygame.font.SysFont("Arial", 11, bold=True)
        except Exception:
            self._font = None
            self._cue_font = None
        self._label_cache: dict[str, Texture] = {}
        self._cue_label_cache: dict = {}  # (letter, fg) -> Texture

        self._beat_tex = self._make_beat_texture()
        self._tri_top, self._tri_bottom = self._make_triangle_textures()
        self._decks: dict[int, Optional[_DeckTexture]] = {i: None for i in range(self.deck_count)}

        # Apply always-on-top via Win32 (the SDL flag is unreliable on Windows).
        self.set_always_on_top(self._on_top)

    def _create_window(self, size, borderless: bool) -> Window:
        """Create a resizable SDL window (borderless optional). Border/size/position
        are all runtime-toggleable so the user can switch between a clean overlay
        and a normal movable/resizable window without recreating anything."""
        title = "rkbx_wave — Waveforms"
        try:
            return Window(title=title, size=size, resizable=True, borderless=borderless)
        except TypeError:
            return Window(title=title, size=size, resizable=True)

    # ------------------------------------------------------------------
    # Window controls (border / always-on-top) — driven from the tk GUI
    # ------------------------------------------------------------------
    def _user32(self):
        u = ctypes.windll.user32
        from ctypes import wintypes
        u.FindWindowW.restype = wintypes.HWND
        u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
        u.SetWindowPos.restype = wintypes.BOOL
        u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
        return u

    def _get_hwnd(self) -> Optional[int]:
        if self._hwnd:
            return self._hwnd
        try:
            self._hwnd = self._user32().FindWindowW(None, self._title) or None
        except Exception:
            self._hwnd = None
        return self._hwnd

    def set_always_on_top(self, enabled: bool) -> None:
        """Make the waveform window topmost (or not) via Win32 SetWindowPos."""
        self._on_top = bool(enabled)
        if sys.platform != "win32":
            return
        try:
            hwnd = self._get_hwnd()
            if not hwnd:
                return
            HWND_TOPMOST, HWND_NOTOPMOST = -1, -2
            SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE = 0x0002, 0x0001, 0x0010
            self._user32().SetWindowPos(
                hwnd, HWND_TOPMOST if enabled else HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            )
        except Exception:
            pass

    def set_borderless(self, borderless: bool) -> None:
        """Toggle the window border live. Bordered = native move/resize; borderless
        = clean overlay. Re-assert topmost afterwards (toggling can drop it)."""
        try:
            self.window.borderless = bool(borderless)
        except Exception:
            pass
        if self._on_top:
            self.set_always_on_top(True)

    def reassert_topmost(self) -> None:
        """Periodically re-apply topmost so it can't be stolen by other windows."""
        if self._on_top:
            self.set_always_on_top(True)

    def bind_display(self, key, fill):
        self._bound_display = key
        self._fill_display = bool(fill)
        self._display_check = 0.0
        self.check_display()

    def check_display(self):
        if not self._bound_display or time.monotonic() < self._display_check:
            return
        self._display_check = time.monotonic() + 2.0
        from .displays import list_displays
        displays = list_displays()
        selected = next((d for d in displays if d.key == self._bound_display), None)
        self.display_available = selected is not None
        target = selected or displays[0]
        bounds = (target.x, target.y, target.width, target.height, self._fill_display)
        if bounds == getattr(self, "_display_bounds", None):
            return
        self._display_bounds = bounds
        self.window.position = (target.x, target.y)
        if self._fill_display:
            self.window.size = (target.width, target.height)
            self.set_borderless(True)

    def _make_beat_texture(self) -> Optional[Texture]:
        """A 3px-wide soft vertical line (alpha falloff at the edges). Drawn at a
        float x with linear filtering it renders as a thin, anti-aliased beat
        marker that slides sub-pixel-smoothly instead of snapping to whole pixels."""
        try:
            c = self.beat_color
            surf = pygame.Surface((3, 1), pygame.SRCALPHA)
            surf.set_at((0, 0), (c.r, c.g, c.b, 70))
            surf.set_at((1, 0), (c.r, c.g, c.b, 255))
            surf.set_at((2, 0), (c.r, c.g, c.b, 70))
            tex = Texture.from_surface(self.renderer, surf)
            tex.blend_mode = pygame.BLENDMODE_BLEND
            return tex
        except Exception:
            return None

    def _make_triangle_textures(self):
        """Two small triangles in the end-marker colour: one pointing down (drawn at
        the top of the beat line) and one pointing up (drawn at the bottom), like the
        red markers Rekordbox draws at the ends of each beat line."""
        try:
            c = self.beat_end_color
            w, h = 9, 6
            top = pygame.Surface((w, h), pygame.SRCALPHA)      # apex pointing down
            pygame.draw.polygon(top, (c.r, c.g, c.b, 255), [(0, 0), (w - 1, 0), ((w - 1) / 2, h - 1)])
            bottom = pygame.Surface((w, h), pygame.SRCALPHA)   # apex pointing up
            pygame.draw.polygon(bottom, (c.r, c.g, c.b, 255), [(0, h - 1), (w - 1, h - 1), ((w - 1) / 2, 0)])
            tt = Texture.from_surface(self.renderer, top)
            bt = Texture.from_surface(self.renderer, bottom)
            tt.blend_mode = pygame.BLENDMODE_BLEND
            bt.blend_mode = pygame.BLENDMODE_BLEND
            return tt, bt
        except Exception:
            return None, None

    def set_beat_style(
        self,
        color: Optional[Tuple[int, int, int]] = None,
        end_color: Optional[Tuple[int, int, int]] = None,
        width: Optional[float] = None,
        triangles: Optional[bool] = None,
    ) -> None:
        """Update beat line colour / end-marker colour / width / markers live (cheap)."""
        if color is not None:
            self.beat_color = pygame.Color(*color)
            self._beat_tex = self._make_beat_texture()
        if end_color is not None:
            self.beat_end_color = pygame.Color(*end_color)
            self._tri_top, self._tri_bottom = self._make_triangle_textures()
        if width is not None:
            self._beat_width = max(0.5, float(width))
        if triangles is not None:
            self._beat_triangles = bool(triangles)

    # ------------------------------------------------------------------
    # Texture management (called on track load / config change)
    # ------------------------------------------------------------------
    def choose_pixels_per_second(self, total_duration: float, base: float = 50.0) -> float:
        """Pick a texture density that keeps the whole-track texture within GPU limits."""
        total_duration = max(float(total_duration), 1e-3)
        max_pps = self.max_track_width / total_duration
        return max(1.0, min(base, max_pps))

    def clamp_pixels_per_second(self, desired: float, total_duration: float) -> float:
        """Clamp a desired pixels-per-second (from zoom) so the texture fits the GPU.

        Sizing the texture near the on-screen resolution per zoom is what kills the
        minification aliasing: the GPU then scales ~1:1 instead of skipping texels.
        """
        total_duration = max(float(total_duration), 1e-3)
        cap = self.max_track_width / total_duration
        return max(0.01, min(float(desired), 150.0, cap))

    def set_track(
        self,
        deck_idx: int,
        image: Image.Image,
        pixels_per_second: float,
        total_duration: float,
        downbeats_sec: Sequence[float] = (),
        cues: Sequence = (),
        beats: Sequence = (),
    ) -> None:
        """Upload a full-track waveform image to a GPU texture for the given deck."""
        if deck_idx not in self._decks:
            return
        tiles = []
        for offset in range(0, image.width, self.max_texture_width):
            tile = image.crop((offset, 0, min(image.width, offset + self.max_texture_width), image.height))
            surface = pygame.image.frombuffer(tile.tobytes(), tile.size, "RGB")
            tiles.append((offset, Texture.from_surface(self.renderer, surface)))
        texture = tiles[0][1]
        self._decks[deck_idx] = _DeckTexture(
            texture=texture,
            tiles=tuple(tiles),
            pixels_per_second=float(pixels_per_second),
            total_duration=max(float(total_duration), 1e-6),
            downbeats=tuple(float(t) for t in downbeats_sec),
            beats=tuple(beats) or tuple((float(t), True) for t in downbeats_sec),
            cues=tuple(cues),
        )

    def clear_track(self, deck_idx: int) -> None:
        self._decks[deck_idx] = None

    # ------------------------------------------------------------------
    # Per-frame rendering
    # ------------------------------------------------------------------
    def render_frame(self, frames: Sequence[DeckFrame], *, present=True, output_size=None) -> None:
        if self._closed:
            return
        w, h = output_size or self.window.size
        deck_h = max(1, h // self.deck_count)

        self.renderer.draw_color = self.background
        self.renderer.clear()

        font_size = max(11, min(24, round(deck_h * 0.065 * self.ui_scale)))
        if font_size != self._font_size:
            self._font_size = font_size
            self._font = pygame.font.SysFont("Helvetica,Arial", font_size)
            self._cue_font = pygame.font.SysFont("Helvetica,Arial", max(10, font_size - 2), bold=True)
            self._label_cache.clear()
            self._cue_label_cache.clear()
        for row, idx in enumerate(self.row_order):
            top = row * deck_h
            viewport = pygame.Rect(0, top, w, deck_h if row < self.deck_count - 1 else h - top)
            frame = frames[idx] if idx < len(frames) else None
            deck = self._decks.get(idx)
            self.renderer.draw_color = pygame.Color(41, 48, 57)
            self.renderer.fill_rect((0, top, w, 1))
            rail = max(38, int(font_size * 2.5))
            header = font_size + 14
            self._text(str(idx + 1), 12, top + header + (deck_h - header) / 2 - font_size / 2, (237, 241, 245))
            if frame is not None and deck is not None and frame.current_time is not None:
                title = frame.label or f"Deck {idx + 1}"
                # Title is clipped to its own region so it never overlaps BPM.
                old = self.renderer.get_viewport()
                self.renderer.set_viewport(pygame.Rect(rail, top, max(1, w - rail - 150), header))
                self._text(title, 0, 6, (166, 177, 189))
                self.renderer.set_viewport(old)
                if frame.bpm:
                    self._text(f"{frame.bpm:.2f} BPM", w - 138, top + 6, (237, 241, 245))
                signal = pygame.Rect(rail, top + header, max(1, w - rail), max(1, viewport.height - header - 4))
                self._draw_deck(deck, frame, signal)
            else:
                self._text("Waiting for track", rail, top + deck_h / 2 - font_size / 2, (166, 177, 189))
        if present:
            self.renderer.present()

    def _text(self, text, x, y, color=(237, 241, 245)):
        if self._font is None:
            return
        key = (text, color)
        tex = self._label_cache.get(key)
        if tex is None:
            if len(self._label_cache) >= 128:
                self._label_cache.clear()
            tex = Texture.from_surface(self.renderer, self._font.render(text, True, color))
            tex.blend_mode = pygame.BLENDMODE_BLEND
            self._label_cache[key] = tex
        tex.draw(dstrect=(x, y, tex.width, tex.height))

    def _draw_deck(self, deck: _DeckTexture, frame: DeckFrame, viewport: pygame.Rect) -> None:
        visible = max(frame.visible_seconds, 1e-3)
        start_time = frame.current_time - visible / 2.0
        px_per_sec = viewport.width / visible
        vh = float(viewport.height)
        flag_h = max(16, self._font_size + 2)
        bottom = vh - flag_h - 3
        signal_h = max(1, bottom - flag_h)
        prev_vp = self.renderer.get_viewport()
        self.renderer.set_viewport(viewport)
        try:
            full_w = deck.total_duration * px_per_sec
            for offset, texture in deck.tiles:
                x = (offset / deck.pixels_per_second - start_time) * px_per_sec
                width = texture.width / deck.pixels_per_second * px_per_sec
                if x + width >= 0 and x <= viewport.width:
                    texture.draw(dstrect=(x, flag_h, width, signal_h))
            lo, hi = start_time, start_time + visible
            # Full grid: first beat is red, remaining beats are quiet white ticks.
            for bt, downbeat in deck.beats:
                if not lo <= bt <= hi:
                    continue
                bx = (bt - start_time) * px_per_sec
                self.renderer.draw_color = self.beat_end_color if downbeat else self.beat_color
                bw = self._beat_width if downbeat else max(1, self._beat_width / 2)
                tick = 12 if downbeat else 5
                self.renderer.fill_rect((bx - bw / 2, flag_h, bw, tick))
                self.renderer.fill_rect((bx - bw / 2, bottom - tick, bw, tick))
                if downbeat:
                    self.renderer.fill_rect((bx - 0.5, flag_h + tick, 1, max(1, signal_h - tick * 2)))
                    if self._beat_triangles and self._tri_top:
                        self._tri_top.draw(dstrect=(bx - 4, flag_h, 9, 6))
            occupied = []
            for cue in deck.cues:
                ct, color, is_memory, number = cue[:4]
                end = cue[4] if len(cue) > 4 else None
                self.renderer.draw_color = pygame.Color(*color)
                if end is not None and end > ct and end >= lo and ct <= hi:
                    left = max(0, (ct - lo) * px_per_sec)
                    right = min(viewport.width, (end - lo) * px_per_sec)
                    self.renderer.fill_rect((left, bottom + 4, max(1, right - left), 2))
                    for edge in (ct, end):
                        ex = (edge - lo) * px_per_sec
                        if 0 <= ex <= viewport.width:
                            self.renderer.fill_rect((ex, bottom, 2, flag_h - 2))
                if not lo <= ct <= hi:
                    continue
                cx = (ct - start_time) * px_per_sec
                self.renderer.fill_rect((cx, flag_h, 1 if is_memory else 2, signal_h))
                # Stagger neighbouring labels instead of painting one over another.
                fw = flag_h + (8 if end else 0)
                placement = place_cue_flag(cx, fw, viewport.width, occupied)
                if placement is None:
                    continue  # Stems remain visible when every flag position is occupied.
                fx, row = placement
                fy = row * flag_h
                if abs(fx - cx) > 1:
                    self.renderer.fill_rect((min(cx, fx), fy + flag_h - 1, abs(fx - cx) + 1, 1))
                self.renderer.fill_rect((fx, fy, fw, flag_h))
                letter = ("ABCDEFGH"[number - 1] if not is_memory and 1 <= number <= 8 else "M")
                if end:
                    letter += "L"
                fg = (7, 9, 12) if sum(color) > 380 else (255, 255, 255)
                lt = self._cue_letter_texture(letter, fg)
                if lt:
                    lt.draw(dstrect=(fx + (fw - lt.width) / 2, fy + (flag_h - lt.height) / 2, lt.width, lt.height))
            # One fixed rail spans the same relative position in every row.
            cx = viewport.width / 2
            self.renderer.draw_color = self.playhead_color
            self.renderer.fill_rect((cx - 1, 0, 2, vh))
            self.renderer.fill_rect((cx - 4, 0, 8, 3))
        finally:
            self.renderer.set_viewport(prev_vp)

    def _cue_letter_texture(self, letter: str, fg: Tuple[int, int, int]):
        """Cached small bold letter texture for a hot-cue square (A-H)."""
        if self._cue_font is None:
            return None
        key = (letter, fg)
        tex = self._cue_label_cache.get(key)
        if tex is None:
            surf = self._cue_font.render(letter, True, fg)
            tex = Texture.from_surface(self.renderer, surf)
            tex.blend_mode = pygame.BLENDMODE_BLEND
            self._cue_label_cache[key] = tex
        return tex

    def _draw_label(self, text: str, viewport: pygame.Rect) -> None:
        if self._font is None:
            return
        tex = self._label_cache.get(text)
        if tex is None:
            surf = self._font.render(text, True, (220, 220, 220))
            tex = Texture.from_surface(self.renderer, surf)
            self._label_cache[text] = tex
        dst = pygame.Rect(viewport.x + 6, viewport.y + 4, tex.width, tex.height)
        tex.draw(dstrect=dst)

    # ------------------------------------------------------------------
    # Event pump / lifecycle
    # ------------------------------------------------------------------
    def pump(self) -> None:
        """Process window events; invoke on_close when the user closes the window."""
        if self._closed:
            return
        self.check_display()
        for event in pygame.event.get():
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    self._fill_display = False
                    self.set_borderless(False)
                elif event.key in (pygame.K_PLUS, pygame.K_EQUALS, pygame.K_KP_PLUS):
                    self.zoom_delta += 1
                elif event.key in (pygame.K_MINUS, pygame.K_KP_MINUS):
                    self.zoom_delta -= 1
            if event.type == pygame.QUIT:
                self._request_close()
            elif event.type == getattr(pygame, "WINDOWEVENT", -1):
                # Window close button on some platforms arrives as a WINDOWEVENT.
                if getattr(event, "event", None) == getattr(pygame, "WINDOWEVENT_CLOSE", -2):
                    self._request_close()

    def _request_close(self) -> None:
        if self._closed:
            return
        if self.on_close is not None:
            self.on_close()
        else:
            self.close()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            pygame.quit()
        except Exception:
            pass
