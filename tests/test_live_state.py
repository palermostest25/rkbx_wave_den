import math
import unittest

from rb_waveform_core.live_state import PlaybackTimeline


class PlaybackTimelineTests(unittest.TestCase):
    def test_unloaded_reset_and_pause(self):
        timeline = PlaybackTimeline()
        self.assertIsNone(timeline.position(0))
        self.assertTrue(timeline.stale(0))
        timeline.observe(5, observed_at=1, playing=True, rate=1)
        self.assertAlmostEqual(timeline.position(1.02), 5.02)
        timeline.observe(5.02, observed_at=1.02, playing=False, rate=1)
        self.assertEqual(timeline.position(1.2), 5.02)
        timeline.reset()
        self.assertIsNone(timeline.position(2))

    def test_velocity_interpolation_and_stale_cap(self):
        timeline = PlaybackTimeline()
        for index in range(5):
            timeline.observe(10 + index * .02, observed_at=1 + index * .02)
        self.assertAlmostEqual(timeline.position(1.10), 10.10)
        self.assertAlmostEqual(timeline.position(9), 10.16)
        self.assertTrue(timeline.stale(9))

    def test_unchanged_observations_stop_after_bounded_poll_hold(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=0)
        timeline.observe(5.02, observed_at=.02)
        timeline.observe(5.02, observed_at=.04)
        self.assertAlmostEqual(timeline.position(.06), 5.06)
        timeline.observe(5.02, observed_at=.12)
        self.assertEqual(timeline.position(.20), 5.02)

    def test_polling_faster_than_source_updates_does_not_stutter(self):
        timeline = PlaybackTimeline()
        for tick in range(30):
            now = tick * .02
            position = (tick // 2) * .04
            timeline.observe(position, observed_at=now)
            if tick >= 2:
                self.assertAlmostEqual(timeline.position(now), now)
                self.assertAlmostEqual(timeline.position(now + .01), now + .01)
        self.assertFalse(timeline.observe(1, observed_at=now - .001))

    def test_cue_jump_is_not_a_huge_velocity(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=0)
        timeline.observe(5.02, observed_at=.02)
        timeline.observe(80, observed_at=.04)
        self.assertEqual(timeline.position(.10), 80)
        timeline.observe(80.02, observed_at=.06)
        self.assertAlmostEqual(timeline.position(.08), 80.04)

    def test_reverse_jog_follows_without_overshooting(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=0)
        timeline.observe(4.98, observed_at=.02)
        self.assertAlmostEqual(timeline.position(.03), 4.99)  # glides between real samples
        self.assertAlmostEqual(timeline.position(.2), 4.98)   # and never runs past the platter

    def test_platter_scratch_never_runs_ahead(self):
        # Mac tenths clock, hand moving the platter back and forth.
        import math
        timeline = PlaybackTimeline(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)
        timeline.rate_hint = 1.0
        for tick in range(240):
            now = tick / 60
            timeline.observe(math.floor((30 + .35 * math.sin(3 * math.pi * now)) * 10) / 10, observed_at=now)
            if tick > 30:
                truth = 30 + .35 * math.sin(3 * math.pi * now)
                self.assertLess(abs(timeline.position(now) - truth), .2)

    def mac_run(self, truth, seconds, controller=False, play_pressed=None):
        timeline = PlaybackTimeline(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)
        timeline.rate_hint, timeline.controller, timeline.play_pressed = 1.0, controller, play_pressed
        shown, last = [], None
        for tick in range(int(seconds * 60)):
            now = tick / 60
            position = math.floor(truth(now) * 10) / 10
            timeline.observe(position, observed_at=(last[1] + now) / 2 if last and last[0] != position else now)
            last = (position, now)
            shown.append((timeline.position(now), truth(now)))
        return shown

    def test_tiny_jog_wiggle_on_a_clock_edge_does_not_jitter(self):
        shown = self.mac_run(lambda t: 30.1 + .03 * math.sin(2 * math.pi * 1.2 * t), 4)[60:]
        self.assertLess(max(abs(a - b) for a, b in shown), .05)
        self.assertLess(max(abs(b[0] - a[0]) for a, b in zip(shown, shown[1:])), .02)

    def test_controller_tells_a_nudge_from_play(self):
        nudge = lambda t: 30.05 + (.06 * (t - 1) if t > 1 else 0) - (.06 * (t - 2) if t > 2 else 0)
        shown = self.mac_run(nudge, 3, controller=True)
        self.assertLess(max(a - b for a, b in shown), .06)  # never runs ahead of the hand
        play = lambda t: 30.05 + max(0, t - 1)
        shown = self.mac_run(play, 3, controller=True, play_pressed=.98)
        self.assertLess(max(abs(a - b) for a, b in shown[90:]), .03)  # PLAY: smooth prediction

    def test_play_stop_and_back_to_start_is_not_a_loop(self):
        # Play 32 beats (an exact power of two), stop, return to the start: CUE, not a loop.
        for mac, pause in ((False, 0), (False, 1.0), (True, 0), (True, .2), (True, 1.0)):
            timeline = (PlaybackTimeline(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)
                        if mac else PlaybackTimeline())
            timeline.rate_hint, timeline.beat_seconds = 1.0, .5
            step = 1 / 60 if mac else .02
            now = 0.0
            while now < 16:
                timeline.observe(math.floor(now * 10) / 10 if mac else now, observed_at=now)
                now += step
            stopped = math.floor(now * 10) / 10 if mac else now
            end = now + pause
            while now < end:  # paused: the same position repeats
                timeline.observe(stopped, observed_at=now)
                now += step
            for _ in range(120):  # back at the start, still paused
                timeline.observe(0.0, observed_at=now)
                self.assertIsNone(timeline.active_loop(), (mac, pause))
                now += step

    def test_mouse_loop_shows_from_its_first_repeat(self):
        timeline = PlaybackTimeline()
        timeline.rate_hint, timeline.beat_seconds = 1.0, .5
        now = 0.0
        while now < 6:  # a 2-second (4-beat) loop from 2.0 engaged at 3.9
            timeline.observe(now if now < 4 else 2 + (now - 4) % 2, observed_at=now)
            now += .02
            if 4.2 <= now < 5.9:  # the session then snaps the edges to the beat grid
                self.assertTrue(timeline.active_loop())
                for edge, expected in zip(timeline.active_loop(), (2.0, 4.0)):
                    self.assertAlmostEqual(edge, expected, delta=.03)

    def test_steady_playback_is_not_mistaken_for_the_platter(self):
        timeline = PlaybackTimeline(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)
        timeline.rate_hint = 1.06
        for tick in range(300):
            now = tick / 60
            timeline.observe(math.floor(10 * 1.06 * now) / 10, observed_at=now)
            self.assertFalse(tick > 20 and timeline.scrubbing(now))

    def test_authoritative_loop_wraps_and_saved_loop_does_not(self):
        timeline = PlaybackTimeline()
        timeline.observe(11.99, observed_at=0, playing=True, rate=1, loop_start=10, loop_end=12, loop_active=True)
        self.assertAlmostEqual(timeline.position(.03), 10.02)
        timeline.observe(11.99, observed_at=.1, playing=True, rate=1, loop_start=10, loop_end=12)
        self.assertAlmostEqual(timeline.position(.13), 12.02)

    def test_wrap_observation_keeps_forward_motion(self):
        timeline = PlaybackTimeline()
        timeline.observe(11.99, observed_at=0, loop_start=10, loop_end=12, loop_active=True)
        timeline.observe(10.01, observed_at=.02, loop_start=10, loop_end=12, loop_active=True)
        self.assertAlmostEqual(timeline.position(.04), 10.03)

    def test_invalid_and_out_of_order_samples_do_not_corrupt_state(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=1)
        for position, stamp in [(math.nan, 2), (8, math.inf), ('bad', 2), (8, .9), (8, 1)]:
            self.assertFalse(timeline.observe(position, observed_at=stamp))
        self.assertEqual(timeline.position(2), 5)
        self.assertEqual(timeline.position(math.nan), 5)

    def test_tenths_clock_scrolls_at_an_even_speed(self):
        # A Mac UI clock polled at ~60 Hz: the display must neither lurch nor step back.
        import random
        from rb_waveform_core.mac_accessibility_reader import edge_stamp
        rnd = random.Random(7)
        timeline = PlaybackTimeline(extrapolate_seconds=.3, stale_seconds=.6, resolution=.1, smoothing_seconds=.6)
        timeline.rate_hint = 1.0
        now, last, read, frames = 0.0, None, 0.0, []
        while now < 6:
            now += .001
            if now >= read:
                value = math.floor((30 + now) * 10) / 10
                timeline.observe(value, observed_at=edge_stamp(last, value, now))
                last, read = (value, now), now + rnd.uniform(.012, .022)
            if now > 2 and round(now * 1000) % 17 == 0:
                frames.append((now, timeline.position(now)))
        self.assertLess(max(abs(p - 30 - t) for t, p in frames), .01)
        speeds = [(b[1] - a[1]) / (b[0] - a[0]) for a, b in zip(frames, frames[1:])]
        self.assertGreater(min(speeds), .9)
        self.assertLess(max(speeds), 1.1)

    def test_small_errors_are_eased_not_snapped(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=0)
        timeline.observe(5.02, observed_at=.02)
        before = timeline.position(.04)
        timeline.observe(5.05, observed_at=.04)
        self.assertAlmostEqual(timeline.position(.04), before)
        # The correction decays; the display converges on the sampled motion.
        self.assertAlmostEqual(timeline.position(5), 5.05 + .08 * timeline.rate)

    def test_bpm_hint_starts_motion_at_the_true_speed(self):
        timeline = PlaybackTimeline(extrapolate_seconds=.3, resolution=.1)
        timeline.rate_hint = .92
        timeline.observe(10, observed_at=0)
        timeline.observe(10, observed_at=.4)
        self.assertAlmostEqual(timeline.position(.5), 10.05)  # paused: middle of the tenth
        timeline.observe(10.1, observed_at=.5)
        self.assertAlmostEqual(timeline.rate, .92)

    def test_repeated_jump_back_is_shown_as_a_loop(self):
        timeline = PlaybackTimeline()
        timeline.beat_seconds = .5
        now, pos = 0.0, 20.0
        for _ in range(3):
            for _ in range(50):
                timeline.observe(pos, observed_at=now)
                now, pos = now + .02, pos + .02
            pos -= 1.0  # a two-beat loop at 0.5 s per beat
        start, end = timeline.active_loop()
        self.assertAlmostEqual(end - start, 1.0, delta=.05)
        self.assertLessEqual(start, pos)
        # Playing on past the loop end clears it.
        for _ in range(80):
            timeline.observe(pos, observed_at=now)
            now, pos = now + .02, pos + .02
        self.assertIsNone(timeline.active_loop())

    def test_single_hot_cue_jump_is_not_a_loop(self):
        timeline = PlaybackTimeline()
        timeline.beat_seconds = .5
        for index in range(10):
            timeline.observe(50 + index * .02, observed_at=index * .02)
        timeline.observe(31.37, observed_at=.2)
        self.assertIsNone(timeline.active_loop())


if __name__ == '__main__':
    unittest.main()
