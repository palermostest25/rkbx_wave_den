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


class WaveformTiles:
    """Demand-loaded native-resolution tiles; memory stays bounded for long tracks."""
    def __init__(self, capacity=64):
        self.images = OrderedDict()
        self.capacity = capacity

    def clear(self):
        self.images.clear()

    def get(self, analysis, tile, colors, render, reduction=1):
        key = (id(analysis), tile, reduction, tuple(colors.low_color), tuple(colors.mid_color),
               tuple(colors.high_color), colors.rgb_mode, render.low_gain,
               render.mid_gain, render.high_gain, render.smoothing_bins)
        if key in self.images:
            self.images.move_to_end(key)
            return self.images[key]
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
        result = QImage(raw, image.width, image.height, image.width * 3, QImage.Format.Format_RGB888).copy()
        self.images[key] = result
        while len(self.images) > self.capacity:
            self.images.popitem(last=False)
        return result


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
        for row, idx in enumerate(order):
            self.draw_deck(painter, idx, QRectF(0, row * height, self.width(), height))
        painter.end()

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
            for tile in range(first, last + 1):
                reduction = max(1, 2 ** max(0, math.floor(math.log2(bin_rate / max(pps, .001)))))
                image = self.tiles.get(analysis, tile, s.colors, s.render, reduction)
                if image is not None:
                    target = QRectF(x_at(tile * 1024 / bin_rate), signal.top(),
                                    min(1024, bins - tile * 1024) / bin_rate * pps, signal.height())
                    p.drawImage(target, image)
        timeline = s.timelines[idx]
        if s.view.get('loops', True) and timeline.loop_active and timeline.loop_start is not None:
            left, right = x_at(timeline.loop_start), x_at(timeline.loop_end)
            rect = QRectF(left, signal.top(), right - left, signal.height())
            p.fillRect(rect, QColor(255, 143, 0, 45))
            p.setPen(QPen(QColor('#ff9c00'), 2))
            p.drawRect(rect)
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
            fw = max(20, font_size * 1.2)
            placements = flag_positions([x_at(c.time_sec) - signal.left() for c in cues], fw, signal.width())
            for cue, placement in zip(cues, placements):
                x = x_at(cue.time_sec)
                color = QColor(*cue.color)
                # Stem is always at the real cue position, regardless of label collision.
                p.setPen(QPen(color, 1))
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
                    p.setPen(QPen(QColor(*cue.color), 2))
                    p.drawLine(QPointF(left, y), QPointF(right, y))
                    p.drawLine(QPointF(left, y - 4), QPointF(left, y + 5))
                    p.drawLine(QPointF(right, y - 4), QPointF(right, y + 5))
        # A white playhead is distinct from red first-beat ticks.
        x = x_at(position)
        p.setPen(QPen(QColor('#060606'), 5))
        p.drawLine(QPointF(x, bounds.top() + header + 2), QPointF(x, bounds.bottom() - 4))
        p.setPen(QPen(QColor('#f8f8f8'), 2.2))
        p.drawLine(QPointF(x, bounds.top() + header + 2), QPointF(x, bounds.bottom() - 4))
