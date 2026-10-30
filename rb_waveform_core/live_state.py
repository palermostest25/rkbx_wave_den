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
        # Set when a controller is heard: its PLAY/CUE presses tell motor starts from hand nudges.
        self.controller = False
        self.play_pressed: Optional[float] = None
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
        self._candidate = None  # a jump that looks like a loop wrap, until playback carries on
        # Controller loop presses: a pressed IN awaiting OUT, and the loop RELOOP restores.
        self.pending_in: Optional[float] = None
        self.last_loop = None
        # Platter use (scratch, jog cueing): until then the display follows samples.
        self.scrub_until: Optional[float] = None
        self._glide = None  # (start time, start position, duration) toward the latest sample
        self._fresh = False  # first motion after a stop: play, or a slow jog?
        self._paced = False  # the last tick moved at playback pace
        self._spun_at = -1e9  # last fast backward step: another soon after is a spin, not a seek
        self._wrap_end = None  # where rekordbox last wrapped, when that differed from the loop drawn
        self._interval: Optional[float] = None  # typical time between changed samples
        self._proven = True  # the drawn loop's wrap can be predicted
        # Where in the clock quantum the deck is: a hand-moved tick is a crossing of its lower
        # edge going up or its upper edge going down. None: unknown, so the middle.
        self._quantum: Optional[float] = None
        self._ticked = 0.0

    def started(self, now):
        """PLAY or CUE pressed on the controller just before this motion."""
        return self.play_pressed is not None and 0 <= now - self.play_pressed < .6

    def scrubbing(self, now):
        return self.scrub_until is not None and now < self.scrub_until

    def engage(self, start, end):
        """A loop the DJ just engaged; the observed wraps keep correcting it."""
        self.inferred_loop = self.last_loop = (start, end)
        self.pending_in = self._jump = self._candidate = self._wrap_end = None
        self._proven = True  # the controller said so

    def exit_loop(self):
        self.inferred_loop = self._candidate = None

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
        # A loop keeps playing after it jumps back; CUE or a jump to the start stops, so
        # either kind of match only becomes a loop once playback carries on from it.
        if (previous and abs(previous[0] - start) < .12 and abs(previous[1] - end) < .12
                and stamp - previous[2] < 2 * (end - start) + .5):
            self._candidate = ((previous[0] + start) / 2, (previous[1] + end) / 2, stamp)
            return
        beat = self.beat_seconds
        if beat and end - start > 0:
            beats = 2 ** round(math.log2((end - start) / beat))
            # A hot-cue jump rarely spans an exact power-of-two beat count.
            if 1 / 32 <= beats <= 64 and abs((end - start) - beats * beat) < .06 + self.resolution:
                self._candidate = (start, start + beats * beat, stamp)

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
            reach = 4.0 * elapsed + 0.02 + self.resolution  # how far the deck can move, clock step included
            if loop and delta < 0 and loop[0] - self.resolution - .05 <= pos <= loop[0] + reach:
                wrapped = delta + loop[1] - loop[0]
                if not -self.resolution <= wrapped <= reach and self.loop_active is None and old_rate > .2:
                    # rekordbox wrapped at another point than the loop drawn. Twice at the same
                    # point means the loop is that long; once may just be RELOOP from inside it.
                    end = self.sample_position + elapsed * old_rate - (pos - loop[0])
                    if self._wrap_end is not None and abs(self._wrap_end - end) < .12 + self.resolution:
                        self.inferred_loop = loop = (loop[0], (self._wrap_end + end) / 2)
                        self._proven = True  # seen wrapping there twice
                        wrapped = delta + loop[1] - loop[0]
                    self._wrap_end = end
                if -self.resolution <= wrapped <= reach:
                    delta = wrapped
                    self._proven = True  # rekordbox wrapped where the loop is drawn
            # One clock quantum can tick at any moment, so a single tick is never a seek.
            discontinuity = elapsed > self.stale_seconds or (
                abs(delta) > 4.0 * elapsed + 0.02 and abs(delta) > self.resolution + 1e-6)
            by_hand = self.scrubbing(stamp)  # before this sample: a spin starting now is not yet hand motion
            if (not discontinuity and delta < 0 and self._paced and not by_hand
                    and abs(delta) > 2 * self.resolution + .02):
                discontinuity = True  # straight out of steady play, a hand can't pull back that far: a jump
            fast_back = delta < 0 and abs(delta) > 4.0 * elapsed + 0.02 and abs(delta) > self.resolution + 1e-6
            spin_speed = fast_back and abs(delta) < 15 * elapsed + .02 + self.resolution
            if spin_speed and stamp - self._spun_at < .25:
                discontinuity = False  # another fast step backwards: a spin of the platter, not a seek
            if fast_back:
                self._spun_at = stamp
            if spin_speed:
                self.scrub_until = stamp + .5  # glide into it: a seek this short looks the same
            if self._candidate:
                start, end, jumped = self._candidate
                if discontinuity or delta <= 0 or not start - .25 <= pos <= end or stamp - jumped > 1:
                    self._candidate = None  # stopped, moved away or jumped again: not a loop
                elif stamp - jumped >= .08:
                    self.inferred_loop, self._candidate = (start, end), None
                    self._proven = False  # one jump back could be RELOOP: draw it, don't predict it yet
            # Hands on the platter: backwards, reversing, or not moving like playback.
            expected = old_rate * elapsed
            pace = _finite(self.rate_hint) or old_rate
            # One tick of a coarse clock says little about speed: playback needs two in a row
            # at pace, each within the clock's timing error (a tick lands up to a poll early or late).
            at_pace = bool(pace) and abs(delta - pace * elapsed) < .15 * pace * elapsed + .3 * self.resolution + .005
            steady, self._paced = at_pace and self._paced, at_pace if delta else self._paced
            if not discontinuity and delta and (delta < 0 or delta * old_rate < 0 or (
                    old_rate and abs(delta - expected) > self.resolution + .03 + .25 * abs(expected))
                    or (self.scrubbing(stamp) and not steady)  # still by hand, not the motor
                    or (old_rate == 0 and self.controller and not self.started(stamp))):
                self.scrub_until = stamp + .5
            inside = loop and loop[0] - .25 <= pos <= loop[1]  # entering the known loop (RELOOP)
            if (discontinuity and delta < 0 and old_rate > .2 and elapsed <= self.stale_seconds
                    and self.loop_active is None and not by_hand and not inside):
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
        if (hint and self.rate and old_rate and not discontinuity and len(self._samples) >= 4
                and abs(self.rate / hint - 1) > .3):
            # Far from the track's playback speed: a hand on the platter, not the motor.
            self.scrub_until = stamp + .5
        if (explicit_rate is None and hint and 0 < hint <= 4.0 and self.rate > 0
                and not discontinuity and (old_rate == 0 or abs(self.rate / hint - 1) < .15)):
            self.rate = hint
        if explicit_rate is not None and self.playing is not False:
            self.rate = explicit_rate
        scrubbing = self.scrubbing(stamp)
        if scrubbing:
            self._samples.clear()  # scratch motion would poison the playback-rate fit
        if discontinuity:
            self._quantum = None
        elif delta:
            # Slow hand moves sit at the edge just crossed; fast ones are anywhere in between.
            slow = scrubbing and stamp - self._ticked > .15
            self._quantum = (0.0 if delta > 0 else self.resolution) if slow else None
            self._ticked = stamp
        self._glide = None
        if self.observed_at is not None:
            gap = stamp - self.observed_at
            if delta and not discontinuity:
                self._interval = gap if self._interval is None else .7 * self._interval + .3 * gap
            # Glide over a little more than a typical interval, so a late sample doesn't stall it.
            span = min(max(1.25 * (self._interval or gap), 1e-3), .25)
            if before is not None and (scrubbing or not discontinuity):
                self._glide = (stamp, before, span)
        self._fresh = old_rate == 0 and self.rate != 0 and not discontinuity
        self.sample_position, self.observed_at = pos, stamp
        settled = (discontinuity or before is None or scrubbing or old_rate == 0
                   or self.rate == 0 or self.playing is False)
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
        if self.scrubbing(stamp) and self._glide:
            # Glide from where the display was toward the latest sample over one sample
            # interval: it never runs ahead of the platter, so it never has to come back.
            t0, p0, span = self._glide
            k = min(1.0, max(0.0, (stamp - t0) / span))
            return p0 + (self.sample_position + self._within(stamp) - p0) * k
        elapsed = max(0.0, min(since, self.extrapolate_seconds))
        rate = 0.0 if self.playing is False else self.rate
        if self._fresh and rate:
            # PLAY's next tick arrives within one step; a slow jog's doesn't. Don't guess further.
            elapsed = min(elapsed, max(self.resolution, .04) / abs(rate))
        pos = self.sample_position + elapsed * rate
        if rate == 0:
            pos += self._within(stamp)
        if self._offset:
            pos += self._offset * math.exp(-max(0.0, since) / self.smoothing_seconds)
        loop = self.active_loop()
        if loop and (self._proven or self.loop_active) and loop[0] <= self.sample_position < loop[1]:
            if pos >= loop[1] or pos < loop[0]:
                pos = loop[0] + (pos - loop[0]) % (loop[1] - loop[0])
        return pos

    def _within(self, now):
        # The crossed edge right after a tick; with no tick since, anywhere in the quantum.
        if self._quantum is None:
            return self.resolution / 2
        fade = min(1.0, max(0.0, now - self._ticked) / .6)
        return self._quantum + (self.resolution / 2 - self._quantum) * fade

    def stale(self, now=None):
        if self.observed_at is None:
            return True
        stamp = _finite(time.monotonic() if now is None else now)
        return stamp is None or stamp - self.observed_at > self.stale_seconds
