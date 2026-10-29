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

    def test_reverse_jog_is_supported(self):
        timeline = PlaybackTimeline()
        timeline.observe(5, observed_at=0)
        timeline.observe(4.98, observed_at=.02)
        self.assertAlmostEqual(timeline.position(.04), 4.96)

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
