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
    repeated positions briefly retain interpolation across producer ticks. A discontinuity resets
    the estimate, so cue jumps do not become enormous scrolling velocities.

    Small disagreements between the display and a new sample are eased out over
    ``smoothing_seconds`` instead of snapping, so polling jitter never shows as
    a jerk. ``resolution`` is the source's position quantum (Mac UI clocks show
    tenths); a stopped deck is shown mid-quantum rather than at its floor.
    """

    def __init__(self, *, extrapolate_seconds=0.08, stale_seconds=0.25, resolution=0.0,
                 smoothing_seconds=0.3):
        self.extrapolate_seconds = max(0.0, float(extrapolate_seconds))
        self.stale_seconds = max(self.extrapolate_seconds, float(stale_seconds))
        self.resolution = max(0.0, float(resolution))
        self.smoothing_seconds = max(1e-3, float(smoothing_seconds))
        # Live/original BPM ratio supplied by the session; exact where polling is coarse.
        self.rate_hint: Optional[float] = None
        # Seconds per beat, used to accept a loop from its first wrap.
        self.beat_seconds: Optional[float] = None
        self.reset()

    def reset(self):
        self.sample_position: Optional[float] = None
        self.observed_at: Optional[float] = None
        self._received_at: Optional[float] = None
        self.playing: Optional[bool] = None
        self.rate = 0.0
        self.loop_active: Optional[bool] = None
        self.loop_start: Optional[float] = None
        self.loop_end: Optional[float] = None
        self._samples = deque(maxlen=12)
        self._offset = 0.0
        self._jump = None
        self.inferred_loop = None
        # Controller loop presses: a pressed IN awaiting OUT, and the loop RELOOP restores.
        self.pending_in: Optional[float] = None
        self.last_loop = None

    def engage(self, start, end):
        """A loop the DJ just engaged; the observed wraps keep correcting it."""
        self.inferred_loop = self.last_loop = (start, end)
        self.pending_in = self._jump = None

    def exit_loop(self):
        self.inferred_loop = None

    def active_loop(self):
        """Engaged loop bounds: authoritative when the source reports loops, else inferred."""
        if self.loop_active and self.loop_start is not None:
            return self.loop_start, self.loop_end
        if self.loop_active is None and self.inferred_loop:
            return self.inferred_loop
        return None

    def _note_jump(self, start, end, stamp):
        """Readers without loop state still reveal a loop: it jumps back by the same span."""
        previous, self._jump = self._jump, (start, end, stamp)
        if (previous and abs(previous[0] - start) < .12 and abs(previous[1] - end) < .12
                and stamp - previous[2] < 2 * (end - start) + .5):
            self.inferred_loop = ((previous[0] + start) / 2, (previous[1] + end) / 2)
            return
        beat = self.beat_seconds
        if beat and end - start > 0:
            beats = 2 ** round(math.log2((end - start) / beat))
            # A hot-cue jump rarely spans an exact power-of-two beat count.
            if 1 / 32 <= beats <= 64 and abs((end - start) - beats * beat) < .05 + .02 * beats * beat:
                self.inferred_loop = (start, start + beats * beat)

    def observe(self, position, *, observed_at=None, playing=None, rate=None,
                loop_start=None, loop_end=None, loop_active=None):
        """Accept a source observation; return False for invalid/stale samples."""
        pos = _finite(position)
        stamp = _finite(time.monotonic() if observed_at is None else observed_at)
        if pos is None or stamp is None:
            return False
        if self._received_at is not None and stamp <= self._received_at:
            return False
        self._received_at = stamp
        before = self.position(stamp)
        old_rate = self.rate

        start, end = _finite(loop_start), _finite(loop_end)
        valid_loop = start is not None and end is not None and end > start
        self.loop_active = bool(loop_active) if loop_active is not None else None
        self.loop_start, self.loop_end = (start, end) if valid_loop else (None, None)
        self.playing = bool(playing) if playing is not None else None
        explicit_rate = _finite(rate)
        if explicit_rate is not None and abs(explicit_rate) > 4.0:
            explicit_rate = None

        # Polls can repeat a moving source's last position. Re-anchoring each one
        # makes the waveform jump backwards and falsely infer a pause every tick.
        if (self.observed_at is not None and pos == self.sample_position
                and self.playing is not False
                and stamp - self.observed_at < self.extrapolate_seconds):
            if explicit_rate is not None:
                self.rate = explicit_rate
            return True

        discontinuity = self.observed_at is None
        delta = None
        if self.observed_at is not None:
            elapsed = stamp - self.observed_at
            delta = pos - self.sample_position
            # A known engaged loop can wrap without being a seek. Preserve the
            # incoming rate, but still anchor at the actual sampled position.
            loop = self.active_loop()
            if loop and delta < 0:
                wrapped = delta + loop[1] - loop[0]
                if 0 <= wrapped <= 4.0 * elapsed + 0.02:
                    delta = wrapped
            discontinuity = elapsed > self.stale_seconds or abs(delta) > 4.0 * elapsed + 0.02
            if discontinuity and delta < 0 and old_rate > .2 and elapsed <= self.stale_seconds and self.loop_active is None:
                # The jump happened somewhere in this interval; both sides advanced equally since.
                start = pos - elapsed * old_rate / 2
                self._note_jump(start, start + self.sample_position + elapsed * old_rate - pos, stamp)
            # A quantized loop may start up to half a beat ahead of the press.
            if self.inferred_loop and not self.inferred_loop[0] - .6 <= pos <= self.inferred_loop[1] + .25:
                self.inferred_loop = None
            if self.pending_in is not None and pos < self.pending_in - .6:
                self.pending_in = None

        if discontinuity or self.playing is False or delta == 0:
            self._samples.clear()
            self.rate = 0.0
        else:
            # Regression cannot cross a loop boundary: absolute positions wrap; nor
            # start from a pause anchor, which is not when motion resumed.
            if (pos < self.sample_position and self.active_loop()) or old_rate == 0:
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
        hint = _finite(self.rate_hint)
        if (explicit_rate is None and hint and 0 < hint <= 4.0 and self.rate > 0
                and not discontinuity and (old_rate == 0 or abs(self.rate / hint - 1) < .15)):
            self.rate = hint
        if explicit_rate is not None and self.playing is not False:
            self.rate = explicit_rate
        self.sample_position, self.observed_at = pos, stamp
        settled = discontinuity or before is None or old_rate == 0 or self.rate == 0 or self.playing is False
        offset = 0.0 if settled else before - pos
        self._offset = offset if abs(offset) < .2 else 0.0
        return True

    def position(self, now=None):
        """Return current display position, or None before a source observation."""
        if self.sample_position is None:
            return None
        stamp = _finite(time.monotonic() if now is None else now)
        if stamp is None:
            return self.sample_position
        since = stamp - self.observed_at
        elapsed = max(0.0, min(since, self.extrapolate_seconds))
        rate = 0.0 if self.playing is False else self.rate
        pos = self.sample_position + elapsed * rate
        if rate == 0:
            pos += self.resolution / 2
        if self._offset:
            pos += self._offset * math.exp(-max(0.0, since) / self.smoothing_seconds)
        loop = self.active_loop()
        if loop and loop[0] <= self.sample_position < loop[1]:
            if pos >= loop[1] or pos < loop[0]:
                pos = loop[0] + (pos - loop[0]) % (loop[1] - loop[0])
        return pos

    def stale(self, now=None):
        if self.observed_at is None:
            return True
        stamp = _finite(time.monotonic() if now is None else now)
        return stamp is None or stamp - self.observed_at > self.stale_seconds
