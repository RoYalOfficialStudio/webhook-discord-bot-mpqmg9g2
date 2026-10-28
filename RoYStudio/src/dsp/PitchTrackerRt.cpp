#include "dsp/PitchTrackerRt.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

void PitchTrackerRt::prepare(double sampleRate, double minHz, double maxHz) {
    sr_ = sampleRate;
    minHz_ = minHz;
    maxHz_ = maxHz;
    decim_ = std::max(1, static_cast<int>(std::floor(sr_ / 16000.0)));
    fs_ = sr_ / decim_;
    for (auto& f : aa_) f.set(Biquad::Type::LowPass, sr_, std::min(0.42 * fs_, 7000.0), 0.7071);
    W_ = static_cast<int>(std::lround(0.024 * fs_));
    maxLag_ = static_cast<int>(std::ceil(fs_ / minHz_));
    minLag_ = std::max(2, static_cast<int>(std::floor(fs_ / maxHz_)));
    hop_ = std::max(16, static_cast<int>(std::lround(0.004 * fs_)));
    lagsPerSample_ = (maxLag_ + hop_ - 1) / hop_ + 1;
    ring_.assign(static_cast<size_t>(W_ + maxLag_ + 2) * 2, 0.0f);
    frame_.assign(static_cast<size_t>(W_ + maxLag_ + 2), 0.0f);
    diff_.assign(static_cast<size_t>(maxLag_ + 2), 1.0f);
    raw_.assign(static_cast<size_t>(maxLag_ + 2), 0.0f);
    reset();
}

void PitchTrackerRt::reset() {
    for (auto& f : aa_) f.reset();
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    ringPos_ = ringFill_ = 0;
    decimCount_ = sinceHop_ = 0;
    frameActive_ = false;
    voiced_ = false;
    voicedRun_ = 0;
    hz_ = midi_ = conf_ = lastMidi_ = 0;
}

bool PitchTrackerRt::push(float x) noexcept {
    const float y = aa_[1].process(aa_[0].process(x));
    if (++decimCount_ < decim_) return false;
    decimCount_ = 0;
    const int rs = static_cast<int>(ring_.size());
    ring_[static_cast<size_t>(ringPos_)] = y;
    ringPos_ = (ringPos_ + 1) % rs;
    ringFill_ = std::min(rs, ringFill_ + 1);
    bool done = false;
    if (frameActive_) {
        work(lagsPerSample_);
        if (lagCursor_ > maxLag_) {
            finish();
            frameActive_ = false;
            done = true;
        }
    }
    if (++sinceHop_ >= hop_) {
        sinceHop_ = 0;
        if (!frameActive_ && ringFill_ >= W_ + maxLag_) startFrame();
    }
    return done;
}

void PitchTrackerRt::startFrame() noexcept {
    const int total = W_ + maxLag_;
    const int rs = static_cast<int>(ring_.size());
    for (int k = 0; k < total; ++k) frame_[static_cast<size_t>(k)] = ring_[static_cast<size_t>((ringPos_ - total + k + rs * 4) % rs)];
    energy_ = 0;
    for (int k = 0; k < W_; ++k) energy_ += static_cast<double>(frame_[static_cast<size_t>(k)]) * frame_[static_cast<size_t>(k)];
    diff_[0] = 1.0f;
    running_ = 0;
    lagCursor_ = 1;
    frameActive_ = true;
}

void PitchTrackerRt::work(int lags) noexcept {
    for (int n = 0; n < lags && lagCursor_ <= maxLag_; ++n, ++lagCursor_) {
        const int tau = lagCursor_;
        double d = 0;
        for (int k = 0; k < W_; ++k) {
            const double q = frame_[static_cast<size_t>(k)] - frame_[static_cast<size_t>(k + tau)];
            d += q * q;
        }
        running_ += d;
        raw_[static_cast<size_t>(tau)] = static_cast<float>(d);
        diff_[static_cast<size_t>(tau)] = static_cast<float>(running_ > 0 ? d * tau / running_ : 1.0);
    }
}

void PitchTrackerRt::finish() noexcept {
    const double rmsLevel = std::sqrt(energy_ / W_);
    int best = -1;
    for (int tau = minLag_; tau < maxLag_; ++tau)
        if (diff_[static_cast<size_t>(tau)] < 0.2f) {
            while (tau + 1 < maxLag_ && diff_[static_cast<size_t>(tau + 1)] < diff_[static_cast<size_t>(tau)]) ++tau;
            best = tau;
            break;
        }
    if (best < 0) { // no dip below the absolute threshold: global minimum gives the (low) confidence
        float m = 2.0f;
        for (int tau = minLag_; tau < maxLag_; ++tau)
            if (diff_[static_cast<size_t>(tau)] < m) {
                m = diff_[static_cast<size_t>(tau)];
                best = tau;
            }
    }
    double conf = 0, midi = midi_;
    if (best > minLag_ && best < maxLag_ - 1 && rmsLevel > 3e-4) { // level gate about -70 dBFS
        // sub-sample period: parabola through the RAW difference (less biased than through d')
        double tauF = best;
        const double y0 = raw_[static_cast<size_t>(best - 1)], y1 = raw_[static_cast<size_t>(best)], y2 = raw_[static_cast<size_t>(best + 1)];
        const double den = y0 - 2 * y1 + y2;
        if (std::fabs(den) > 1e-12) tauF += std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
        conf = std::clamp(1.0 - static_cast<double>(diff_[static_cast<size_t>(best)]), 0.0, 1.0);
        midi = hzToMidi(fs_ / tauF);
    }
    // octave / outlier guard: a sudden jump > 7 semitones needs high confidence
    if (voiced_ && std::fabs(midi - lastMidi_) > 7.0 && conf < 0.92) conf = std::min(conf, 0.5);
    // hysteresis
    if (conf >= 0.8) {
        if (++voicedRun_ >= 2) voiced_ = true;
    } else if (conf < 0.6) {
        voicedRun_ = 0;
        voiced_ = false;
    }
    conf_ = conf;
    if (conf >= 0.6) {
        midi_ = midi;
        hz_ = midiToHz(midi);
        lastMidi_ = midi;
    }
}

} // namespace roy::dsp
