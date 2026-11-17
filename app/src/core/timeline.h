// Playback interpolation driven by observed transport data, never saved cues: follows Rekordbox's sampled
// position smoothly between samples, tells seeks from loop wraps and platter use, and eases out small
// disagreements instead of snapping. All times are monotonic seconds. A line-by-line port of
// rb_waveform_core/live_state.py, whose behaviour was tuned against real decks.
#pragma once

#include <deque>
#include <optional>
#include <utility>

namespace wave {

using Loop = std::pair<double, double>;

struct Observation {
    double position = 0;
    std::optional<double> observedAt, rate, loopStart, loopEnd;
    std::optional<bool> playing, loopActive;
};

class PlaybackTimeline {
public:
    struct Timing { double extrapolate = .08, stale = .25, resolution = 0.0, smoothing = .3; };

    explicit PlaybackTimeline(bool inferLoops = true) : inferLoops(inferLoops) { reset(); }

    void reset();
    void timing(const Timing &t);
    bool observe(const Observation &o, double now);
    std::optional<double> position(double now) const;
    bool stale(double now) const { return !observedAt_ || now - *observedAt_ > stale_; }
    std::optional<Loop> activeLoop() const;
    void engage(double start, double end);
    void exitLoop() { inferredLoop.reset(); candidate_.reset(); }
    bool started(double now) const { return playPressed && now - *playPressed >= 0 && now - *playPressed < .6; }
    bool scrubbing(double now) const { return scrubUntil && now < *scrubUntil; }

    // As the session sets them.
    bool inferLoops = true;
    std::optional<double> rateHint;      // live / original BPM, exact where polling is coarse
    std::optional<double> beatSeconds;   // to accept a loop from its first wrap
    bool controller = false;             // a controller is heard: PLAY/CUE tell motor starts from nudges
    std::optional<double> playPressed;

    // What the display reads.
    std::optional<double> samplePosition;
    std::optional<bool> playing;
    double rate = 0;
    std::optional<bool> loopActive;
    std::optional<double> loopStart, loopEnd;
    std::optional<Loop> inferredLoop, lastLoop;
    std::optional<double> pendingIn, scrubUntil;
    double resolution() const { return resolution_; }

private:
    void noteJump(double start, double end, double stamp);
    double within(double now) const;

    double extrapolate_ = .08, stale_ = .25, resolution_ = 0, smoothing_ = .3;
    std::optional<double> observedAt_, receivedAt_;
    std::deque<std::pair<double, double>> samples_;
    double offset_ = 0;
    struct Jump { double start, end, stamp; };
    std::optional<Jump> jump_;
    std::optional<Jump> candidate_;
    struct Glide { double start, from, span; };
    std::optional<Glide> glide_;
    bool fresh_ = false, paced_ = false, proven_ = true;
    double spunAt_ = -1e9;
    std::optional<double> wrapEnd_, interval_, quantum_;
    double ticked_ = 0;
};

}  // namespace wave
