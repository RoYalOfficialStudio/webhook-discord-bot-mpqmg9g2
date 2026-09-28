#pragma once
// Core realtime DSP building blocks. All process methods are allocation-free.
#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace roy::dsp {

// RBJ "Audio EQ Cookbook" biquad, transposed direct form II, double state.
class Biquad {
public:
    enum class Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf, AllPass };
    void set(Type t, double sr, double freq, double q, double gainDb = 0.0) noexcept {
        freq = std::clamp(freq, 1.0, sr * 0.49);
        q = std::max(0.025, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = kTwoPi * freq / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (t) {
        case Type::LowPass: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::BandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::Notch: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::AllPass: b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::Peak: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case Type::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case Type::HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        }
        b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
    }
    float process(float x) noexcept {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        z1_ = sanitize(z1_);
        z2_ = sanitize(z2_);
        return static_cast<float>(y);
    }
    void reset() noexcept { z1_ = z2_ = 0; }
    // Magnitude response at `freq` (for tests / EQ display).
    double magnitudeAt(double freq, double sr) const {
        const double w = kTwoPi * freq / sr;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
        return std::abs((b0_ + b1_ * z1 + b2_ * z2) / (1.0 + a1_ * z1 + a2_ * z2));
    }

private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_ = 0, z2_ = 0;
};

// Zero-delay-feedback state variable filter (Simper/TPT). Stable under fast modulation.
class Svf {
public:
    void set(double sr, double freq, double q) noexcept {
        freq = std::clamp(freq, 5.0, sr * 0.49);
        g_ = std::tan(kPi * freq / sr);
        k_ = 1.0 / std::max(0.05, q);
        a1_ = 1.0 / (1.0 + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
    struct Out { float lp, bp, hp; };
    Out process(float x) noexcept {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = sanitize(2 * v1 - ic1_);
        ic2_ = sanitize(2 * v2 - ic2_);
        return {static_cast<float>(v2), static_cast<float>(v1), static_cast<float>(x - k_ * v1 - v2)};
    }
    void reset() noexcept { ic1_ = ic2_ = 0; }

private:
    double g_ = 0, k_ = 1, a1_ = 0, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
};

// One-pole smoother for parameters / envelopes.
class Smoother {
public:
    void setTime(double sr, double seconds) noexcept { coef_ = seconds <= 0 ? 0.0 : std::exp(-1.0 / (seconds * sr)); }
    void reset(float v) noexcept { y_ = v; }
    float next(float target) noexcept {
        y_ = target + (y_ - target) * coef_;
        return static_cast<float>(y_);
    }
    float value() const noexcept { return static_cast<float>(y_); }

private:
    double coef_ = 0, y_ = 0;
};

// Envelope follower with separate attack/release (linear magnitude in, out).
class EnvelopeFollower {
public:
    void set(double sr, double attackSec, double releaseSec) noexcept {
        att_ = attackSec <= 0 ? 0.0 : std::exp(-1.0 / (attackSec * sr));
        rel_ = releaseSec <= 0 ? 0.0 : std::exp(-1.0 / (releaseSec * sr));
    }
    float process(float x) noexcept {
        const double c = x > env_ ? att_ : rel_;
        env_ = sanitize(x + (env_ - x) * c);
        return static_cast<float>(env_);
    }
    void reset() noexcept { env_ = 0; }
    float value() const noexcept { return static_cast<float>(env_); }

private:
    double att_ = 0, rel_ = 0, env_ = 0;
};

class Adsr {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    void set(double sr, double a, double d, double s, double r) noexcept {
        sr_ = sr;
        attackInc_ = a <= 0 ? 1.0 : 1.0 / (a * sr);
        decayCoef_ = d <= 0 ? 0.0 : std::exp(-5.0 / (d * sr));
        sustain_ = std::clamp(s, 0.0, 1.0);
        releaseCoef_ = r <= 0 ? 0.0 : std::exp(-5.0 / (r * sr));
    }
    void noteOn(bool retrigger = true) noexcept {
        if (retrigger || stage_ == Stage::Idle) {
            stage_ = Stage::Attack;
        } else {
            stage_ = Stage::Attack;
        }
    }
    void noteOff() noexcept {
        if (stage_ != Stage::Idle) stage_ = Stage::Release;
    }
    void kill() noexcept {
        stage_ = Stage::Idle;
        v_ = 0;
    }
    float next() noexcept {
        switch (stage_) {
        case Stage::Idle: return 0.0f;
        case Stage::Attack:
            v_ += attackInc_;
            if (v_ >= 1.0) { v_ = 1.0; stage_ = Stage::Decay; }
            break;
        case Stage::Decay:
            v_ = sustain_ + (v_ - sustain_) * decayCoef_;
            if (v_ - sustain_ < 1e-4) { v_ = sustain_; stage_ = Stage::Sustain; }
            break;
        case Stage::Sustain: v_ = sustain_; break;
        case Stage::Release:
            v_ *= releaseCoef_;
            if (v_ < 1e-5) { v_ = 0; stage_ = Stage::Idle; }
            break;
        }
        return static_cast<float>(v_);
    }
    bool active() const noexcept { return stage_ != Stage::Idle; }
    Stage stage() const noexcept { return stage_; }
    float value() const noexcept { return static_cast<float>(v_); }

private:
    double sr_ = 48000, attackInc_ = 1, decayCoef_ = 0, sustain_ = 1, releaseCoef_ = 0, v_ = 0;
    Stage stage_ = Stage::Idle;
};

class DcBlocker {
public:
    void set(double sr, double hz = 10.0) noexcept { r_ = 1.0 - kTwoPi * hz / sr; }
    float process(float x) noexcept {
        const double y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = sanitize(y);
        return static_cast<float>(y);
    }
    void reset() noexcept { x1_ = y1_ = 0; }

private:
    double r_ = 0.999, x1_ = 0, y1_ = 0;
};

// PolyBLEP residual for band-limited saw/square.
inline double polyBlep(double t, double dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

} // namespace roy::dsp
