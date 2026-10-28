"""Playback interpolation driven by observed transport data, never saved cues.

All timestamps passed to this module are monotonic seconds. Loop state is used
only when a live reader supplies it; a saved loop does not imply engagement.
"""
from __future__ import annotations

import math
import time
from collections import deque
from typing import Optional


def _finite(value):
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return number if math.isfinite(number) else None


class PlaybackTimeline:
    """Follow sampled playback with bounded, seek-aware interpolation.

    ``observe`` accepts authoritative ``playing``, ``rate`` and loop fields when
    available. Without these fields, velocity is measured over a short window;
    repeated positions stop interpolation immediately. A discontinuity resets
    the estimate, so cue jumps do not become enormous scrolling velocities.
    """

    def __init__(self, *, extrapolate_seconds=0.08, stale_seconds=0.25):
        self.extrapolate_seconds = max(0.0, float(extrapolate_seconds))
        self.stale_seconds = max(self.extrapolate_seconds, float(stale_seconds))
        self.reset()

    def reset(self):
        self.sample_position: Optional[float] = None
        self.observed_at: Optional[float] = None
        self.playing: Optional[bool] = None
        self.rate = 0.0
        self.loop_active: Optional[bool] = None
        self.loop_start: Optional[float] = None
        self.loop_end: Optional[float] = None
        self._samples = deque(maxlen=6)

    def observe(self, position, *, observed_at=None, playing=None, rate=None,
                loop_start=None, loop_end=None, loop_active=None):
        """Accept a source observation; return False for invalid/stale samples."""
        pos = _finite(position)
        stamp = _finite(time.monotonic() if observed_at is None else observed_at)
        if pos is None or stamp is None:
            return False
        if self.observed_at is not None and stamp <= self.observed_at:
            return False

        start, end = _finite(loop_start), _finite(loop_end)
        valid_loop = start is not None and end is not None and end > start
        self.loop_active = bool(loop_active) if loop_active is not None else None
        self.loop_start, self.loop_end = (start, end) if valid_loop else (None, None)
        self.playing = bool(playing) if playing is not None else None
        explicit_rate = _finite(rate)
        if explicit_rate is not None and abs(explicit_rate) > 4.0:
            explicit_rate = None

        discontinuity = self.observed_at is None
        delta = None
        if self.observed_at is not None:
            elapsed = stamp - self.observed_at
            delta = pos - self.sample_position
            # A known engaged loop can wrap without being a seek. Preserve the
            # incoming rate, but still anchor at the actual sampled position.
            if valid_loop and self.loop_active and delta < 0:
                wrapped = delta + end - start
                if 0 <= wrapped <= 4.0 * elapsed + 0.02:
                    delta = wrapped
            discontinuity = elapsed > self.stale_seconds or abs(delta) > 4.0 * elapsed + 0.02

        if discontinuity or self.playing is False or delta == 0:
            self._samples.clear()
            self.rate = 0.0
        else:
            # Regression cannot cross a loop boundary: absolute positions wrap.
            if pos < self.sample_position and self.loop_active:
                self._samples.clear()
            if delta is not None:
                measured = delta / (stamp - self.observed_at)
                if self.rate and measured * self.rate < 0:
                    self._samples.clear()
                self.rate = measured

        self._samples.append((stamp, pos))
        if len(self._samples) >= 2 and delta != 0 and not discontinuity:
            first_time = self._samples[0][0]
            xs = [sample_time - first_time for sample_time, _ in self._samples]
            ys = [sample_pos for _, sample_pos in self._samples]
            avg_x, avg_y = sum(xs) / len(xs), sum(ys) / len(ys)
            denominator = sum((x - avg_x) ** 2 for x in xs)
            if denominator > 1e-12:
                self.rate = max(-4.0, min(4.0, sum((x - avg_x) * (y - avg_y) for x, y in zip(xs, ys)) / denominator))
        if explicit_rate is not None and self.playing is not False:
            self.rate = explicit_rate
        self.sample_position, self.observed_at = pos, stamp
        return True

    def position(self, now=None):
        """Return current display position, or None before a source observation."""
        if self.sample_position is None:
            return None
        stamp = _finite(time.monotonic() if now is None else now)
        if stamp is None:
            return self.sample_position
        elapsed = max(0.0, min(stamp - self.observed_at, self.extrapolate_seconds))
        pos = self.sample_position + elapsed * (0.0 if self.playing is False else self.rate)
        if self.loop_active and self.loop_start is not None and self.loop_start <= self.sample_position < self.loop_end:
            length = self.loop_end - self.loop_start
            if pos >= self.loop_end or pos < self.loop_start:
                pos = self.loop_start + (pos - self.loop_start) % length
        return pos

    def stale(self, now=None):
        if self.observed_at is None:
            return True
        stamp = _finite(time.monotonic() if now is None else now)
        return stamp is None or stamp - self.observed_at > self.stale_seconds
