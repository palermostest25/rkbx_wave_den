"""AZ-style waveform canvas and native, DPI-aware monitor placement."""
from __future__ import annotations

import bisect
import math
from collections import OrderedDict
from dataclasses import replace

import numpy as np
from PySide6.QtCore import Qt, QRectF, QPointF, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QApplication, QWidget

from .analysis import WaveformAnalysis
from .render import render_waveform_image


def screen_key(screen):
    serial = screen.serialNumber().strip()
    return '|'.join((screen.manufacturer(), screen.model(), serial or screen.name()))


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

    def __init__(self, session, parent=None):
        super().__init__(parent)
        self.session = session
        self.tiles = WaveformTiles()
        self.upcoming = []
        self.setMinimumSize(560, 280)
        self.setMouseTracking(True)
        self.setAccessibleName('Stacked deck waveforms')
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def mouseDoubleClickEvent(self, event):
        self.settingsRequested.emit()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        painter.fillRect(self.rect(), QColor('black'))
        order = self.session.order()
        height = self.height() / len(order)
        self.upcoming = []
        for row, idx in enumerate(order):
            self.draw_deck(painter, idx, QRectF(0, row * height, self.width(), height))
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
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor('#e5e5e5'), 1))
        p.drawRect(badge)
        number_font = QFont(font)
        number_font.setPixelSize(round(font_size * 1.5))
        p.setFont(number_font)
        p.drawText(badge, Qt.AlignmentFlag.AlignCenter, str(idx + 1))
        p.setFont(font)
        if bounds.top() > 0:
            p.setPen(QColor('#252525'))
            p.drawLine(QPointF(0, bounds.top()), QPointF(bounds.right(), bounds.top()))
        header = font_size + 12 if s.view.get('track_headers', False) else 0
        flag_h = max(19, font_size + 4)
        signal = QRectF(rail, bounds.top() + header + flag_h + 3,
                        max(1, bounds.width() - rail - 8),
                        max(1, bounds.height() - header - 2 * flag_h - 14))
        position = s.position(idx)
        if deck.analysis is None or position is None:
            p.setPen(QColor('#aab0b8'))
            waiting = 'Waiting for a track in Rekordbox'
            if not s.demo and not (s.reader and s.reader.connected):
                waiting = s.status or waiting
            text = s.errors.get(idx, waiting)
            p.drawText(signal.adjusted(24, 0, -24, 0), Qt.AlignmentFlag.AlignVCenter | Qt.TextFlag.TextWordWrap, text)
            return
        if header:
            p.setPen(QColor('#c3c8ce'))
            text_rect = QRectF(rail, bounds.top() + 3, signal.width() - 180, header)
            title = p.fontMetrics().elidedText(deck.song_name, Qt.TextElideMode.ElideRight, int(text_rect.width()))
            p.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter, title)
            p.setPen(QColor('white'))
            p.drawText(QRectF(bounds.right() - 180, bounds.top(), 165, header), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                       f'{deck.current_bpm:.1f} BPM' if deck.current_bpm else '')
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
            orange = QColor('#ffa21a')
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
        p.drawLine(QPointF(x, bounds.top() + header + 2), QPointF(x, bounds.bottom() - 4))
        p.setPen(QPen(QColor('#f8f8f8'), 2.2))
        p.drawLine(QPointF(x, bounds.top() + header + 2), QPointF(x, bounds.bottom() - 4))
