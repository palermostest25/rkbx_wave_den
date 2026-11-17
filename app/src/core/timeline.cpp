#include "core/timeline.h"

#include <algorithm>
#include <cmath>

namespace wave {
namespace {
std::optional<double> finite(std::optional<double> v) {
    return v && std::isfinite(*v) ? v : std::nullopt;
}
}  // namespace

void PlaybackTimeline::reset() {
    samplePosition.reset(); observedAt_.reset(); receivedAt_.reset(); playing.reset();
    rate = 0; loopActive.reset(); loopStart.reset(); loopEnd.reset();
    samples_.clear(); offset_ = 0; jump_.reset(); inferredLoop.reset(); candidate_.reset();
    pendingIn.reset(); lastLoop.reset(); scrubUntil.reset(); glide_.reset();
    fresh_ = false; paced_ = false; spunAt_ = -1e9; wrapEnd_.reset(); interval_.reset(); proven_ = true;
    quantum_.reset(); ticked_ = 0;
}

void PlaybackTimeline::timing(const Timing &t) {
    extrapolate_ = std::max(0.0, t.extrapolate);
    stale_ = std::max(extrapolate_, t.stale);
    resolution_ = std::max(0.0, t.resolution);
    smoothing_ = std::max(1e-3, t.smoothing);
}

void PlaybackTimeline::engage(double start, double end) {
    inferredLoop = lastLoop = Loop{start, end};
    pendingIn.reset(); jump_.reset(); candidate_.reset(); wrapEnd_.reset();
    proven_ = true;  // the controller said so
}

std::optional<Loop> PlaybackTimeline::activeLoop() const {
    if (loopActive.value_or(false) && loopStart) return Loop{*loopStart, *loopEnd};
    if (inferLoops && !loopActive && inferredLoop) return inferredLoop;
    return std::nullopt;
}

void PlaybackTimeline::noteJump(double start, double end, double stamp) {
    // Readers without loop state still reveal a loop: it jumps back by the same span.
    if (!inferLoops) return;
    const auto previous = jump_;
    jump_ = Jump{start, end, stamp};
    if (previous && std::abs(previous->start - start) < .12 && std::abs(previous->end - end) < .12 &&
        stamp - previous->stamp < 2 * (end - start) + .5) {
        candidate_ = Jump{(previous->start + start) / 2, (previous->end + end) / 2, stamp};
        return;
    }
    if (beatSeconds && end - start > 0) {
        const double beats = std::pow(2.0, std::round(std::log2((end - start) / *beatSeconds)));
        // A hot-cue jump rarely spans an exact power-of-two beat count.
        if (beats >= 1.0 / 32 && beats <= 64 && std::abs((end - start) - beats * *beatSeconds) < .06 + resolution_)
            candidate_ = Jump{start, start + beats * *beatSeconds, stamp};
    }
}

bool PlaybackTimeline::observe(const Observation &o, double now) {
    const auto pos = finite(o.position);
    const auto stamp = finite(o.observedAt ? o.observedAt : now);
    if (!pos || !stamp) return false;
    if (receivedAt_ && *stamp <= *receivedAt_) return false;
    receivedAt_ = stamp;
    const auto before = position(*stamp);
    const double oldRate = rate;

    const auto start = finite(o.loopStart), end = finite(o.loopEnd);
    const bool validLoop = start && end && *end > *start;
    loopActive = o.loopActive;
    if (validLoop) { loopStart = start; loopEnd = end; } else { loopStart.reset(); loopEnd.reset(); }
    playing = o.playing;
    auto explicitRate = finite(o.rate);
    if (explicitRate && std::abs(*explicitRate) > 4.0) explicitRate.reset();

    // Polls can repeat a moving source's last position: re-anchoring each one would jump backwards.
    if (observedAt_ && *pos == *samplePosition && playing != false && *stamp - *observedAt_ < extrapolate_) {
        if (explicitRate) rate = *explicitRate;
        return true;
    }

    bool discontinuity = !observedAt_;
    std::optional<double> delta;
    double elapsed = 0;
    if (observedAt_) {
        elapsed = *stamp - *observedAt_;
        delta = *pos - *samplePosition;
        // A known engaged loop can wrap without being a seek.
        auto loop = activeLoop();
        const double reach = 4.0 * elapsed + 0.02 + resolution_;
        if (loop && *delta < 0 && loop->first - resolution_ - .05 <= *pos && *pos <= loop->first + reach) {
            double wrapped = *delta + loop->second - loop->first;
            if (!(-resolution_ <= wrapped && wrapped <= reach) && !loopActive && oldRate > .2) {
                // Rekordbox wrapped elsewhere than the loop drawn. Twice at the same point: the loop is that long.
                const double wrapEnd = *samplePosition + elapsed * oldRate - (*pos - loop->first);
                if (wrapEnd_ && std::abs(*wrapEnd_ - wrapEnd) < .12 + resolution_) {
                    inferredLoop = loop = Loop{loop->first, (*wrapEnd_ + wrapEnd) / 2};
                    proven_ = true;
                    wrapped = *delta + loop->second - loop->first;
                }
                wrapEnd_ = wrapEnd;
            }
            if (-resolution_ <= wrapped && wrapped <= reach) {
                delta = wrapped;
                proven_ = true;
            }
        }
        // One clock quantum can tick at any moment, so a single tick is never a seek.
        discontinuity = elapsed > stale_ ||
                        (std::abs(*delta) > 4.0 * elapsed + 0.02 && std::abs(*delta) > resolution_ + 1e-6);
        const bool byHand = scrubbing(*stamp);
        if (!discontinuity && *delta < 0 && paced_ && !byHand && std::abs(*delta) > 2 * resolution_ + .02)
            discontinuity = true;  // straight out of steady play, a hand can't pull back that far: a jump
        const bool fastBack = *delta < 0 && std::abs(*delta) > 4.0 * elapsed + 0.02 && std::abs(*delta) > resolution_ + 1e-6;
        const bool spinSpeed = fastBack && std::abs(*delta) < 15 * elapsed + .02 + resolution_;
        if (spinSpeed && *stamp - spunAt_ < .25) discontinuity = false;  // a spin of the platter, not a seek
        if (fastBack) spunAt_ = *stamp;
        if (spinSpeed) scrubUntil = *stamp + .5;
        if (candidate_) {
            const auto c = *candidate_;
            if (discontinuity || *delta <= 0 || !(c.start - .25 <= *pos && *pos <= c.end) || *stamp - c.stamp > 1) {
                candidate_.reset();
            } else if (*stamp - c.stamp >= .08) {
                inferredLoop = Loop{c.start, c.end};
                candidate_.reset();
                proven_ = false;  // one jump back could be RELOOP: draw it, don't predict it yet
            }
        }
        const double expected = oldRate * elapsed;
        const auto hinted = finite(rateHint);
        const double pace = hinted && *hinted ? *hinted : oldRate;  // Python: rate_hint or old_rate
        const bool atPace = pace && std::abs(*delta - pace * elapsed) < .15 * pace * elapsed + .3 * resolution_ + .005;
        const bool steady = atPace && paced_;
        if (*delta) paced_ = atPace;
        if (!discontinuity && *delta &&
            (*delta < 0 || *delta * oldRate < 0 ||
             (oldRate && std::abs(*delta - expected) > resolution_ + .03 + .25 * std::abs(expected)) ||
             (scrubbing(*stamp) && !steady) || (oldRate == 0 && controller && !started(*stamp))))
            scrubUntil = *stamp + .5;
        const bool inside = loop && loop->first - .25 <= *pos && *pos <= loop->second;
        if (discontinuity && *delta < 0 && oldRate > .2 && elapsed <= stale_ && !loopActive && !byHand && !inside) {
            const double from = *pos - elapsed * oldRate / 2;
            noteJump(from, from + *samplePosition + elapsed * oldRate - *pos, *stamp);
        }
        if (inferredLoop && !(inferredLoop->first - .6 <= *pos && *pos <= inferredLoop->second + .25)) inferredLoop.reset();
        if (pendingIn && *pos < *pendingIn - .6) pendingIn.reset();
    }

    if (discontinuity || playing == false || (delta && *delta == 0)) {
        samples_.clear();
        rate = 0;
    } else {
        if ((*pos < *samplePosition && activeLoop()) || oldRate == 0) samples_.clear();
        if (delta) {
            const double measured = *delta / (*stamp - *observedAt_);
            if (rate && measured * rate < 0) samples_.clear();
            rate = measured;
        }
    }
    samples_.emplace_back(*stamp, *pos);
    if (samples_.size() > 12) samples_.pop_front();
    if (samples_.size() >= 2 && !(delta && *delta == 0) && !discontinuity) {
        const double first = samples_.front().first;
        double ax = 0, ay = 0;
        for (const auto &[t, p] : samples_) { ax += t - first; ay += p; }
        ax /= samples_.size(); ay /= samples_.size();
        double num = 0, den = 0;
        for (const auto &[t, p] : samples_) { num += (t - first - ax) * (p - ay); den += (t - first - ax) * (t - first - ax); }
        if (den > 1e-12) rate = std::clamp(num / den, -4.0, 4.0);
    }
    const auto hint = finite(rateHint);
    if (hint && *hint && rate && oldRate && !discontinuity && samples_.size() >= 4 && std::abs(rate / *hint - 1) > .3)
        scrubUntil = *stamp + .5;  // far from the track's speed: a hand on the platter
    if (!explicitRate && hint && *hint > 0 && *hint <= 4.0 && rate > 0 && !discontinuity &&
        (oldRate == 0 || std::abs(rate / *hint - 1) < .15))
        rate = *hint;
    if (explicitRate && playing != false) rate = *explicitRate;
    const bool scrub = scrubbing(*stamp);
    if (scrub) samples_.clear();
    if (discontinuity) {
        quantum_.reset();
    } else if (delta && *delta) {
        const bool slow = scrub && *stamp - ticked_ > .15;
        quantum_ = slow ? std::optional<double>(*delta > 0 ? 0.0 : resolution_) : std::nullopt;
        ticked_ = *stamp;
    }
    glide_.reset();
    if (observedAt_) {
        const double gap = *stamp - *observedAt_;
        if (delta && *delta && !discontinuity) interval_ = interval_ ? .7 * *interval_ + .3 * gap : gap;
        const double span = std::min(std::max(1.25 * interval_.value_or(gap), 1e-3), .25);
        if (before && (scrub || !discontinuity)) glide_ = Glide{*stamp, *before, span};
    }
    fresh_ = oldRate == 0 && rate != 0 && !discontinuity;
    samplePosition = pos;
    observedAt_ = stamp;
    const bool settled = discontinuity || !before || scrub || oldRate == 0 || rate == 0 || playing == false;
    const double offset = settled ? 0.0 : *before - *pos;
    offset_ = std::abs(offset) < .2 ? offset : 0.0;
    return true;
}

double PlaybackTimeline::within(double now) const {
    // The crossed edge right after a tick; with no tick since, anywhere in the quantum.
    if (!quantum_) return resolution_ / 2;
    const double fade = std::clamp((now - ticked_) / .6, 0.0, 1.0);
    return *quantum_ + (resolution_ / 2 - *quantum_) * fade;
}

std::optional<double> PlaybackTimeline::position(double now) const {
    if (!samplePosition) return std::nullopt;
    if (!std::isfinite(now)) return samplePosition;
    const double since = now - *observedAt_;
    if (scrubbing(now) && glide_) {
        // Glide toward the latest sample over one interval: never ahead of the platter.
        const double k = std::clamp((now - glide_->start) / glide_->span, 0.0, 1.0);
        return glide_->from + (*samplePosition + within(now) - glide_->from) * k;
    }
    double elapsed = std::max(0.0, std::min(since, extrapolate_));
    const double r = playing == false ? 0.0 : rate;
    if (fresh_ && r) elapsed = std::min(elapsed, std::max(resolution_, .04) / std::abs(r));
    double pos = *samplePosition + elapsed * r;
    if (r == 0) pos += within(now);
    if (offset_) pos += offset_ * std::exp(-std::max(0.0, since) / smoothing_);
    const auto loop = activeLoop();
    if (loop && (proven_ || loopActive.value_or(false)) && loop->first <= *samplePosition && *samplePosition < loop->second) {
        if (pos >= loop->second || pos < loop->first) pos = loop->first + std::fmod(pos - loop->first, loop->second - loop->first);
    }
    return pos;
}

}  // namespace wave
