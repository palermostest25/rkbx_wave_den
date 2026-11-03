"""AZ-style waveform canvas and native, DPI-aware monitor placement."""
from __future__ import annotations

import bisect
import time
import math
from collections import OrderedDict
from dataclasses import replace

import numpy as np
from PySide6.QtCore import Qt, QEvent, QRectF, QPointF, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QApplication, QWidget

from .analysis import WaveformAnalysis
from .render import render_waveform_image


def screen_key(screen):
    serial = screen.serialNumber().strip()
    # Windows names screens by port (\\.\DISPLAY2): only fall back to it with no make/model.
    return '|'.join((screen.manufacturer(), screen.model(),
                     serial or ('' if screen.manufacturer() or screen.model() else screen.name())))


def find_screen(key, screens):
    return next((screen for screen in screens if screen_key(screen) == key), None)


def bind_fullscreen(window, screen):
    """Use the full screen geometry, never the taskbar-excluding work area."""
    if not window.windowHandle():
        window.winId()
    window.showNormal()
    window.windowHandle().setScreen(screen)
    window.setGeometry(screen.geometry())
    window.showFullScreen()
    from .windows_window import cover_monitor
    cover_monitor(int(window.winId()))



DECK_BLUE = '#1f8fff'
LOOP_ORANGE = '#ffa21a'


def beat_label(beats):
    """Loop length as rekordbox shows it: 1/4, 1/2, 1, 2 … or 3.5 for a free manual loop."""
    if beats < .97:
        return f'1/{round(1 / beats)}'
    return f'{round(beats)}' if abs(beats - round(beats)) < .05 else f'{beats:.1f}'

class WaveformTiles:
    """Demand-loaded native-resolution tiles; memory stays bounded for long tracks."""
    def __init__(self, capacity=64):
        self.images = OrderedDict()
        self.capacity = capacity

    def clear(self):
        self.images.clear()

    def cached(self, analysis, tile, colors, render, reduction=1):
        entry = self.images.get(self.key(analysis, tile, colors, render, reduction))
        # The stored analysis pins its id, so a new track can never reuse an old tile.
        return entry is not None and entry[0] is analysis

    @staticmethod
    def key(analysis, tile, colors, render, reduction):
        return (id(analysis), tile, reduction, tuple(colors.low_color), tuple(colors.mid_color),
                tuple(colors.high_color), colors.rgb_mode, render.low_gain,
                render.mid_gain, render.high_gain, render.smoothing_bins)

    def get(self, analysis, tile, colors, render, reduction=1):
        key = self.key(analysis, tile, colors, render, reduction)
        if self.cached(analysis, tile, colors, render, reduction):
            self.images.move_to_end(key)
            return self.images[key][1]
        result = self.render_tile(analysis, tile, colors, render, reduction)
        if result is not None:
            self.images[key] = (analysis, result)
            while len(self.images) > self.capacity:
                self.images.popitem(last=False)
        return result

    @staticmethod
    def render_tile(analysis, tile, colors, render, reduction):
        start, end = tile * 1024, min((tile + 1) * 1024, len(analysis.low))
        if end <= start:
            return None
        bands = []
        for band, gain in ((analysis.low, render.low_gain), (analysis.mid, render.mid_gain), (analysis.high, render.high_gain)):
            values = band[start:end] * gain
            window = min(max(1, int(render.smoothing_bins)), len(values))
            if window > 1:
                values = np.convolve(values, np.ones(window) / window, mode='same')
            bands.append(values)
        section = WaveformAnalysis(*bands, (end - start) / 150, 0, 0)
        cfg = replace(render, image_width=math.ceil((end - start) / reduction), image_height=256,
                      margin_px=3, background_color=(0, 0, 0))
        image = render_waveform_image(section, colors, cfg, reduce='max')
        raw = image.tobytes()
        return QImage(raw, image.width, image.height, image.width * 3,
                      QImage.Format.Format_RGB888).convertToFormat(QImage.Format.Format_RGB32)


def flag_positions(anchors, width, maximum):
    """Place nearby cue labels without moving their exact-time stems."""
    occupied = [[], []]
    result = []
    for preferred in anchors:
        choices = []
        for row, intervals in enumerate(occupied):
            xs = [max(0, min(preferred, maximum - width)), 0]
            xs += [x + width + 2 for x in intervals]
            for x in xs:
                if x + width <= maximum and all(x + width + 1 <= old or x >= old + width + 1 for old in intervals):
                    choices.append((abs(x - preferred) + row * width / 2, x, row))
        if not choices:
            result.append(None)
            continue
        _, x, row = min(choices)
        occupied[row].append(x)
        result.append((x, row))
    return result


class WaveformCanvas(QWidget):
    settingsRequested = Signal()
    zoomBy = Signal(float)  # multiply the visible beats

    def __init__(self, session, parent=None):
        super().__init__(parent)
        self.session = session
        self.tiles = WaveformTiles()
        self.info = DeckInfoBar(session)
        self.upcoming = []
        self.setMinimumSize(560, 280)
        self.setMouseTracking(True)
        self.setAccessibleName('Stacked deck waveforms')
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def mouseDoubleClickEvent(self, event):
        self.settingsRequested.emit()

    def wheelEvent(self, event):
        # Two-finger scroll or a mouse wheel: up zooms in, a notch is a quarter of a doubling.
        delta = event.angleDelta().y()
        if delta:
            self.zoomBy.emit(2 ** (-delta / 480))
        event.accept()

    def event(self, event):
        # Trackpad pinch (macOS), spreading the fingers zooms in.
        if event.type() == QEvent.Type.NativeGesture and event.gestureType() == Qt.NativeGestureType.ZoomNativeGesture:
            self.zoomBy.emit(1 / max(.2, 1 + event.value()))
            return True
        return super().event(event)

    def info_height(self, decks):
        if not self.session.view.get('deck_info', True):
            return 0
        wanted = min(340, max(150, self.height() * .3)) * self.session.render.ui_scale
        # The waveforms come first: never squeeze a lane below a readable height.
        return max(0, min(wanted, self.height() - 70 * decks))

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        painter.fillRect(self.rect(), QColor('black'))
        order = self.session.order()
        info = self.info_height(len(order))
        height = (self.height() - info) / len(order)
        self.upcoming = []
        for row, idx in enumerate(order):
            self.draw_deck(painter, idx, QRectF(0, row * height, self.width(), height))
        if info:
            self.info.draw(painter, QRectF(0, self.height() - info, self.width(), info), order)
        painter.end()
        # Render one tile ahead per frame (~3 ms), so four decks never fill a frame at once.
        for analysis, tile, reduction in self.upcoming:
            if tile * 1024 < len(analysis.low) and not self.tiles.cached(analysis, tile, self.session.colors, self.session.render, reduction):
                self.tiles.get(analysis, tile, self.session.colors, self.session.render, reduction)
                break

    def draw_deck(self, p, idx, bounds):
        s = self.session
        deck = s.decks[idx]
        scale = s.render.ui_scale
        lane_h = bounds.height()
        font_size = max(14, min(23, lane_h * .075)) * scale
        font = QFont('Arial')
        font.setPixelSize(round(font_size))
        p.setFont(font)
        rail = max(54, font_size * 2.7)
        p.fillRect(QRectF(0, bounds.top(), rail - 7, bounds.height()), QColor('#101010'))
        badge = QRectF(7, bounds.top() + 14, rail - 21, bounds.height() - 28)
        # Lit while the deck plays, like the deck numbers on the AZ's enlarged waveforms.
        playing = bool(s.timelines[idx].playing)
        p.setBrush(QColor(DECK_BLUE) if playing else Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor(DECK_BLUE if playing else '#5b636d'), 1))
        p.drawRect(badge)
        number_font = QFont(font)
        number_font.setPixelSize(round(font_size * 1.5))
        number_font.setBold(True)
        p.setFont(number_font)
        p.setPen(QColor('white' if playing else '#d6dade'))
        p.drawText(badge, Qt.AlignmentFlag.AlignCenter, str(idx + 1))
        p.setFont(font)
        if bounds.top() > 0:
            p.setPen(QColor('#252525'))
            p.drawLine(QPointF(0, bounds.top()), QPointF(bounds.right(), bounds.top()))
        flag_h = max(19, font_size + 4)
        signal = QRectF(rail, bounds.top() + flag_h + 3,
                        max(1, bounds.width() - rail - 8),
                        max(1, bounds.height() - 2 * flag_h - 14))
        position = s.position(idx)
        if deck.analysis is None or position is None:
            p.setPen(QColor('#aab0b8'))
            waiting = 'Waiting for a track in Rekordbox'
            if not s.demo and not (s.reader and s.reader.connected):
                waiting = s.status or waiting
            text = s.errors.get(idx, waiting)
            p.drawText(signal.adjusted(24, 0, -24, 0), Qt.AlignmentFlag.AlignVCenter | Qt.TextFlag.TextWordWrap, text)
            return
        bpm = deck.original_bpm or deck.current_bpm or 120.0
        visible = s.view['zoom_beats'] * 60 / bpm
        fraction = s.view['playhead']
        start = position - visible * fraction
        end = start + visible
        pps = signal.width() / visible
        def x_at(seconds):
            return signal.left() + (seconds - start) * pps
        p.save()
        p.setClipRect(signal)
        analysis = deck.analysis
        duration = analysis.duration_seconds
        bins = len(analysis.low)
        if duration > 0 and bins:
            bin_rate = bins / duration
            first = max(0, math.floor(start * bin_rate / 1024))
            last = min(math.ceil(bins / 1024) - 1, math.floor(end * bin_rate / 1024))
            reduction = max(1, 2 ** max(0, math.floor(math.log2(bin_rate / max(pps, .001)))))
            self.upcoming += [(analysis, last + 1, reduction), (analysis, first - 1, reduction)] if first > 0 else [(analysis, last + 1, reduction)]
            for tile in range(first, last + 1):
                image = self.tiles.get(analysis, tile, s.colors, s.render, reduction)
                if image is not None:
                    target = QRectF(x_at(tile * 1024 / bin_rate), signal.top(),
                                    min(1024, bins - tile * 1024) / bin_rate * pps, signal.height())
                    p.drawImage(target, image)
        if s.view.get('loops', True):
            # Saved loops tint their span in the cue colour, as on the decks.
            for cue in deck.cues:
                if cue.end_sec is None or cue.end_sec < start or cue.time_sec > end:
                    continue
                left, right = x_at(cue.time_sec), x_at(cue.end_sec)
                tint = QColor(*cue.color)
                tint.setAlpha(55)
                p.fillRect(QRectF(left, signal.top(), right - left, signal.height()), tint)
            timeline = s.timelines[idx]
            loop = timeline.active_loop()
            orange = QColor(LOOP_ORANGE)
            pill_font = QFont(font)
            pill_font.setBold(True)
            pill_font.setPixelSize(round(font_size * .72))
            def pill(x, text):
                p.setFont(pill_font)
                w = p.fontMetrics().horizontalAdvance(text) + 12
                box = QRectF(min(max(x, signal.left() + 2), signal.right() - w - 2), signal.top() + 4, w, pill_font.pixelSize() + 6)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(orange)
                p.drawRoundedRect(box, 4, 4)
                p.setPen(QColor('black'))
                p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)
                p.setFont(font)
            if loop:
                left, right = x_at(loop[0]), x_at(loop[1])
                rect = QRectF(left, signal.top() + 1, right - left, signal.height() - 2)
                p.fillRect(rect, QColor(255, 143, 0, 62))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.setPen(QPen(orange, 1.5))
                p.drawRect(rect)
                p.setPen(QPen(orange, 3))
                p.drawLine(QPointF(left, rect.top()), QPointF(left, rect.bottom()))
                p.drawLine(QPointF(right, rect.top()), QPointF(right, rect.bottom()))
                beats = (loop[1] - loop[0]) * bpm / 60
                pill(left + 4, beat_label(beats))
            elif timeline.pending_in is not None and timeline.pending_in <= end:
                # IN pressed, OUT not yet: the loop-to-be grows with the playhead.
                left = x_at(timeline.pending_in)
                if position > timeline.pending_in:
                    p.fillRect(QRectF(left, signal.top() + 1, x_at(position) - left, signal.height() - 2), QColor(255, 143, 0, 34))
                p.setPen(QPen(orange, 3))
                p.drawLine(QPointF(left, signal.top()), QPointF(left, signal.bottom()))
                pill(left + 4, 'IN')
        p.restore()
        # Grid ticks sit outside the waveform, matching the AZ enlarged display.
        if s.view.get('grid', True):
            grid = deck.beat_grid_full or []
            for number, beat in enumerate(grid):
                t = beat.time_ms / 1000
                if not start <= t <= end:
                    continue
                x = x_at(t)
                downbeat = beat.beat_number == 1
                p.setPen(QPen(QColor('#f52620' if downbeat else '#b7bcc1'), 1))
                tick = 13 if downbeat else 8
                p.drawLine(QPointF(x, signal.top() - tick - 2), QPointF(x, signal.top() - 2))
                p.drawLine(QPointF(x, signal.bottom() + 2), QPointF(x, signal.bottom() + tick + 2))
                if downbeat and number // 4 % 4 == 3 and s.view.get('bar_numbers', True):
                    p.setPen(QColor('#efefef'))
                    p.drawText(QRectF(x - 15, signal.bottom() + 11, 40, flag_h), Qt.AlignmentFlag.AlignCenter, str(number // 4 + 1))
        if s.view.get('cues', True):
            cues = [cue for cue in deck.cues if start <= cue.time_sec <= end]
            fw = max(24, font_size * 1.35)
            placements = flag_positions([x_at(c.time_sec) - signal.left() for c in cues], fw, signal.width())
            for cue, placement in zip(cues, placements):
                x = x_at(cue.time_sec)
                color = QColor(*cue.color)
                # Stem is always at the real cue position, regardless of label collision.
                p.setPen(QPen(color, 2))
                p.drawLine(QPointF(x, signal.top()), QPointF(x, signal.bottom()))
                if placement is not None:
                    fx, row = placement
                    fx += signal.left()
                    top = signal.top() - flag_h - 3 + row * flag_h
                    p.setPen(Qt.PenStyle.NoPen)
                    p.setBrush(color)
                    p.drawPolygon(QPolygonF([QPointF(fx, top), QPointF(fx + fw, top),
                                             QPointF(fx + fw, top + flag_h - 5),
                                             QPointF(fx + 5, top + flag_h - 5), QPointF(fx, top + flag_h)]))
                    p.setPen(QColor('black') if color.lightness() > 100 else QColor('white'))
                    label = 'M' if cue.is_memory else ('ABCDEFGHIJKLMNOP'[cue.number - 1] if 1 <= cue.number <= 16 else 'C')
                    p.drawText(QRectF(fx, top - 1, fw, flag_h - 3), Qt.AlignmentFlag.AlignCenter, label)
            if s.view.get('loops', True):
                for cue in deck.cues:
                    if cue.end_sec is None or cue.end_sec < start or cue.time_sec > end:
                        continue
                    left = max(signal.left(), x_at(cue.time_sec))
                    right = min(signal.right(), x_at(cue.end_sec))
                    y = signal.bottom() + 4
                    p.setPen(QPen(QColor(*cue.color), 3))
                    p.drawLine(QPointF(left, y), QPointF(right, y))
                    p.drawLine(QPointF(left, y - 4), QPointF(left, y + 5))
                    p.drawLine(QPointF(right, y - 4), QPointF(right, y + 5))
        # A white playhead is distinct from red first-beat ticks.
        x = x_at(position)
        p.setPen(QPen(QColor('#060606'), 5))
        p.drawLine(QPointF(x, bounds.top() + 2), QPointF(x, bounds.bottom() - 4))
        p.setPen(QPen(QColor('#f8f8f8'), 2.2))
        p.drawLine(QPointF(x, bounds.top() + 2), QPointF(x, bounds.bottom() - 4))


def clock(seconds, tenths=True):
    """mm:ss.t as the deck displays show it."""
    seconds = max(0.0, seconds)
    whole = int(seconds * 10) if tenths else int(seconds) * 10
    return f'{whole // 600:02}:{whole // 10 % 60:02}' + (f'.{whole % 10}' if tenths else '')


def countdown(position, cues, bpm):
    """Bars.beats to the next cue ahead, as rekordbox counts down to it; None with none ahead."""
    ahead = [c.time_sec for c in cues if c.time_sec > position + .05]
    if not ahead or not bpm:
        return None
    beats = math.ceil((min(ahead) - position) * bpm / 60 - 1e-6)
    return f'{beats // 4}.{beats % 4}'


class DeckInfoBar:
    """The AZ's lower screen: one panel per deck with its track, tempo, time and overview."""
    WARN_SECONDS = 30  # the decks flash their overview through a track's last 30 seconds

    def __init__(self, session):
        self.session = session
        self.overviews = OrderedDict()  # id(analysis) -> (analysis, colours, image)
        self.artwork = OrderedDict()    # path -> image (None when unreadable)

    def draw(self, p, bounds, order):
        p.fillRect(bounds, QColor('#0b0c0e'))
        p.setPen(QColor('#2a2e33'))
        p.drawLine(bounds.topLeft(), bounds.topRight())
        width = bounds.width() / len(order)
        for column, idx in enumerate(order):
            self.panel(p, idx, QRectF(bounds.left() + column * width, bounds.top(), width, bounds.height()).adjusted(4, 7, -4, -6))

    def overview(self, analysis):
        s = self.session
        colours = (tuple(s.colors.low_color), tuple(s.colors.mid_color), tuple(s.colors.high_color))
        entry = self.overviews.get(id(analysis))
        if entry and entry[0] is analysis and entry[1] == colours:
            return entry[2]
        cfg = replace(s.render, image_width=max(1, min(1600, len(analysis.low))), image_height=96,
                      margin_px=1, background_color=(0, 0, 0))
        # Averaged, as an overview should read: the loudest bin per column paints it all white.
        image = render_waveform_image(analysis, s.colors, cfg, reduce='mean')
        result = QImage(image.tobytes(), image.width, image.height, image.width * 3,
                        QImage.Format.Format_RGB888).convertToFormat(QImage.Format.Format_RGB32)
        self.overviews[id(analysis)] = (analysis, colours, result)
        while len(self.overviews) > 8:
            self.overviews.popitem(last=False)
        return result

    def cover(self, path):
        if path not in self.artwork:
            image = QImage(path)
            self.artwork[path] = None if image.isNull() else image
            while len(self.artwork) > 16:
                self.artwork.popitem(last=False)
        return self.artwork[path]

    @staticmethod
    def font(size, bold=False):
        font = QFont('Arial')
        font.setPixelSize(max(9, round(size)))
        font.setBold(bold)
        return font

    def text(self, p, rect, text, size, colour, align=Qt.AlignmentFlag.AlignLeft, bold=False):
        p.setFont(self.font(size, bold))
        p.setPen(QColor(colour))
        shown = p.fontMetrics().elidedText(text, Qt.TextElideMode.ElideRight, max(1, int(rect.width())))
        p.drawText(rect, align | Qt.AlignmentFlag.AlignVCenter, shown)

    def chip(self, p, x, y, h, text, fill, ink, align_right=False):
        p.setFont(self.font(h * .62, True))
        w = p.fontMetrics().horizontalAdvance(text) + h * .7
        box = QRectF(x - w if align_right else x, y, w, h)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(fill))
        p.drawRoundedRect(box, 3, 3)
        p.setPen(QColor(ink))
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)
        return box

    def panel(self, p, idx, r):
        s = self.session
        deck = s.decks[idx]
        timeline = s.timelines[idx]
        meta = getattr(s, 'meta', {}).get(idx) or {}
        playing = bool(timeline.playing)
        position = s.position(idx)
        loaded = deck.analysis is not None and position is not None
        p.setPen(QPen(QColor('#24282d'), 1))
        p.setBrush(QColor('#131518'))
        p.drawRect(r)
        pad = max(5.0, r.height() * .035)
        inner = r.adjusted(pad, pad, -pad, -pad)
        gap = max(4.0, pad * .8)
        h = inner.height()
        line = h * .13                      # title / artist lines
        big = h * .27                       # artwork, remaining time, BPM
        info = h * .13                      # elapsed, key, tempo, loop, countdown
        top = inner.top()
        # Deck number, lit while playing, beside the title and artist.
        badge = QRectF(inner.left(), top, 2 * line, 2 * line)
        p.setPen(QPen(QColor(DECK_BLUE if playing else '#5b636d'), 1))
        p.setBrush(QColor(DECK_BLUE) if playing else Qt.BrushStyle.NoBrush)
        p.drawRect(badge)
        self.text(p, badge, str(idx + 1), line * 1.3, 'white' if playing else '#d6dade', Qt.AlignmentFlag.AlignCenter, True)
        text_x = badge.right() + gap
        text_w = inner.right() - text_x
        if not loaded:
            self.text(p, QRectF(text_x, top, text_w, line), 'No track loaded', line * .7, '#7d858e')
            if s.errors.get(idx):
                self.text(p, QRectF(text_x, top + line, text_w, line), s.errors[idx], line * .55, '#9aa3ad')
        else:
            # MASTER, SYNC and key lock, dim until Rekordbox reports them on (bridge only).
            status = getattr(s.reader, 'deck_status', {}).get(idx)
            if status is not None:
                chip_h = line * .78
                right = inner.right()
                for label, on, fill, ink in (('MT', status.master_tempo, '#d63a3a', 'white'),
                                             ('SYNC', status.sync, DECK_BLUE, 'white'),
                                             ('MASTER', status.master, LOOP_ORANGE, 'black')):
                    right = self.chip(p, right, top + (line - chip_h) / 2, chip_h, label,
                                      fill if on else '#1c1f23', ink if on else '#4f565e', align_right=True).left() - gap * .6
                text_w = right - gap - text_x
            self.text(p, QRectF(text_x, top, text_w, line), meta.get('title') or deck.song_name or 'Unknown',
                      line * .72, '#f1f3f5', bold=True)
            self.text(p, QRectF(text_x, top + line, text_w, line), meta.get('artist') or '', line * .6, '#9aa3ad')
        row = top + 2 * line + gap
        # Artwork (or an empty sleeve) with the play state on it.
        art = QRectF(inner.left(), row, big, big)
        image = self.cover(meta['artwork']) if loaded and meta.get('artwork') else None
        if image is not None:
            p.drawImage(art, image)
        else:
            p.fillRect(art, QColor('#1c1f23'))
            p.setPen(QPen(QColor('#2c3137'), 1))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(art.center(), big * .3, big * .3)
            p.drawEllipse(art.center(), big * .06, big * .06)
        if loaded:
            g = big * .34
            glyph = QRectF(art.left() + 3, art.bottom() - g - 3, g, g)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 0, 0, 170))
            p.drawRect(glyph)
            p.setBrush(QColor('#34c759' if playing else '#c9ced4'))
            if playing:
                p.drawPolygon(QPolygonF([QPointF(glyph.left() + g * .32, glyph.top() + g * .22),
                                         QPointF(glyph.left() + g * .32, glyph.bottom() - g * .22),
                                         QPointF(glyph.right() - g * .22, glyph.center().y())]))
            else:
                for x in (.28, .56):
                    p.drawRect(QRectF(glyph.left() + g * x, glyph.top() + g * .24, g * .16, g * .52))
        duration = deck.analysis.duration_seconds if loaded else 0
        remaining = max(0.0, duration - position) if loaded else 0
        warn = loaded and duration > 0 and remaining < self.WARN_SECONDS
        blink = warn and playing and time.monotonic() % 1 < .5
        details = row + big + gap * .5
        if loaded:
            original, current = deck.original_bpm, deck.current_bpm or deck.original_bpm
            bpm_w = min(inner.width() * .36, big * 2.6)
            label = big * .26
            # Remaining time, large; BPM, large, at the right.
            time_x = art.right() + gap * 1.5
            self.text(p, QRectF(time_x, row, inner.width(), label), 'REMAIN', label * .8, '#7d858e')
            self.text(p, QRectF(time_x, row + label, inner.right() - bpm_w - time_x, big - label),
                      '-' + clock(remaining), min((big - label) * .9, (inner.right() - bpm_w - time_x) / 4.3),
                      '#ff3b30' if warn else '#ffffff', bold=True)
            self.text(p, QRectF(inner.right() - bpm_w, row, bpm_w, label), 'BPM', label * .8, '#7d858e',
                      Qt.AlignmentFlag.AlignRight)
            self.text(p, QRectF(inner.right() - bpm_w, row + label, bpm_w, big - label),
                      f'{current:.1f}' if current else '--', min((big - label) * .9, bpm_w / 3.2), '#ffffff',
                      Qt.AlignmentFlag.AlignRight, True)
            # Elapsed, key and tempo at the left; loop and countdown to the next cue at the right.
            x = inner.left()
            self.text(p, QRectF(x, details, info * 4.6, info), clock(position), info * .62, '#9aa3ad')
            x += info * 4.6
            chip_h = info * .82
            y = details + (info - chip_h) / 2
            shift = getattr(getattr(s.reader, 'deck_status', {}).get(idx), 'key_shift', 0)
            if meta.get('key'):
                x = self.chip(p, x, y, chip_h, meta['key'] + (f' {shift:+d}' if shift else ''),
                              '#1f3a2a', '#7ee2a8').right() + gap
            if original and current:
                pct = (current / original - 1) * 100
                self.text(p, QRectF(x, details, info * 4, info), f'{pct:+.2f}%', info * .62,
                          '#f1f3f5' if abs(pct) >= .005 else '#7d858e')
            right = inner.right()
            loop = timeline.active_loop()
            if loop and original:
                right = self.chip(p, right, y, chip_h, 'LOOP ' + beat_label((loop[1] - loop[0]) * original / 60),
                                  LOOP_ORANGE, 'black', align_right=True).left() - gap
            bars = countdown(position, deck.cues, original)
            if bars:
                self.text(p, QRectF(right - info * 5, details, info * 5, info), f'{bars} Bars', info * .62, '#d6dade',
                          Qt.AlignmentFlag.AlignRight)
        overview = QRectF(inner.left(), details + info + gap * .5, inner.width(), inner.bottom() - details - info - gap * .5)
        # Whole-track overview: played part dimmed, cues above and below, the playhead white.
        p.fillRect(overview, QColor('#050506'))
        if loaded and duration > 0:
            p.drawImage(overview, self.overview(deck.analysis))
            def x_at(t):
                return overview.left() + max(0.0, min(1.0, t / duration)) * overview.width()
            p.fillRect(QRectF(overview.left(), overview.top(), x_at(position) - overview.left(), overview.height()),
                       QColor(0, 0, 0, 135))
            for cue in deck.cues:
                if cue.end_sec is not None:
                    tint = QColor(*cue.color)
                    tint.setAlpha(70)
                    p.fillRect(QRectF(x_at(cue.time_sec), overview.top(), x_at(cue.end_sec) - x_at(cue.time_sec), overview.height()), tint)
            loop = timeline.active_loop()
            if loop:
                p.fillRect(QRectF(x_at(loop[0]), overview.top(), max(2.0, x_at(loop[1]) - x_at(loop[0])), overview.height()),
                           QColor(255, 143, 0, 110))
            # Minute ticks along the bottom edge.
            p.setPen(QPen(QColor('#4a5058'), 1))
            for minute in range(1, int(duration // 60) + 1):
                x = x_at(minute * 60)
                p.drawLine(QPointF(x, overview.bottom() - 4), QPointF(x, overview.bottom()))
            mark = max(5.0, overview.height() * .16)
            for cue in deck.cues:
                x = x_at(cue.time_sec)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QColor(*cue.color))
                if cue.is_memory:
                    p.drawPolygon(QPolygonF([QPointF(x - mark * .6, overview.bottom()), QPointF(x + mark * .6, overview.bottom()),
                                             QPointF(x, overview.bottom() - mark)]))
                else:
                    p.drawPolygon(QPolygonF([QPointF(x - mark * .6, overview.top()), QPointF(x + mark * .6, overview.top()),
                                             QPointF(x, overview.top() + mark)]))
            x = x_at(position)
            p.setPen(QPen(QColor('#060606'), 4))
            p.drawLine(QPointF(x, overview.top()), QPointF(x, overview.bottom()))
            p.setPen(QPen(QColor('#ffffff'), 2))
            p.drawLine(QPointF(x, overview.top()), QPointF(x, overview.bottom()))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor('#ff3b30' if blink else '#2a2e33'), 2 if blink else 1))
        p.drawRect(overview)
