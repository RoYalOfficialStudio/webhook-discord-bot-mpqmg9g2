#include "effects/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace roy {

// ---------------------------------------------------------------- helpers
float softClip(float x, float c, float s) noexcept {
    const float a = std::fabs(x);
    if (s <= 0.0f) return std::clamp(x, -c, c);
    const float k = c * (1.0f - std::clamp(s, 0.0f, 1.0f));
    if (a <= k) return x;
    const float y = k + (c - k) * std::tanh((a - k) / std::max(1e-9f, c - k));
    return x < 0 ? -y : y;
}

// ---------------------------------------------------------------- compressor
RoyCompressor::RoyCompressor()
    : Processor({{"threshold", "Threshold", -60, 0, -18, "dB"},
                 {"ratio", "Ratio", 1, 20, 4, ":1"},
                 {"attack", "Attack", 0.05f, 200, 10, "ms"},
                 {"release", "Release", 5, 2000, 120, "ms"},
                 {"knee", "Knee", 0, 24, 6, "dB"},
                 {"makeup", "Makeup", -12, 24, 0, "dB"},
                 {"mix", "Mix", 0, 1, 1},
                 {"sidechain", "External Sidechain", 0, 1, 0, "", 2},
                 {"scHighPass", "Sidechain HPF", 20, 500, 20, "Hz"}}) {}

void RoyCompressor::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoyCompressor::reset() {
    env_ = 0;
    for (auto& f : scHp_) f.reset();
}

double RoyCompressor::curveDb(double x) const {
    const double T = p(Threshold), R = std::max(1.0f, p(Ratio)), W = p(Knee);
    if (2.0 * (x - T) < -W) return x;
    if (W > 0 && 2.0 * std::fabs(x - T) <= W) return x + (1.0 / R - 1.0) * (x - T + W / 2) * (x - T + W / 2) / (2.0 * W);
    return T + (x - T) / R;
}

void RoyCompressor::process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const double att = std::exp(-1.0 / (std::max(0.05, static_cast<double>(p(Attack))) * 0.001 * sr));
    const double rel = std::exp(-1.0 / (std::max(1.0, static_cast<double>(p(Release))) * 0.001 * sr));
    const float makeup = dbToGain(p(Makeup));
    const float mix = p(Mix);
    const bool useSc = p(UseSidechain) > 0.5f && sc != nullptr;
    for (auto& f : scHp_) f.set(dsp::Biquad::Type::HighPass, sr, p(ScHighPass), 0.707);
    float* L = io.channel(0);
    float* R = io.channel(1);
    float maxGr = 0;
    for (int i = 0; i < io.numFrames; ++i) {
        float det;
        if (useSc) det = std::max(std::fabs(scHp_[0].process(sc->channel(0)[i])), std::fabs(scHp_[1].process(sc->channel(1)[i])));
        else det = std::max(std::fabs(L[i]), std::fabs(R[i]));
        const double inDb = gainToDb(static_cast<double>(det), -120.0);
        const double target = std::min(0.0, curveDb(inDb) - inDb); // <= 0
        env_ = target < env_ ? target + (env_ - target) * att : target + (env_ - target) * rel;
        env_ = sanitize(env_);
        const float g = static_cast<float>(dbToGain(env_)) * makeup;
        L[i] = L[i] * (1.0f - mix) + L[i] * g * mix;
        R[i] = R[i] * (1.0f - mix) + R[i] * g * mix;
        maxGr = std::min(maxGr, static_cast<float>(env_));
    }
    gr_.store(maxGr, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- limiter
RoyLimiter::RoyLimiter()
    : Processor({{"ceiling", "Ceiling", -24, 0, -1, "dB"},
                 {"release", "Release", 1, 1000, 60, "ms"},
                 {"lookahead", "Lookahead", 0.5f, 10, 1.5f, "ms"},
                 {"truePeak", "True Peak", 0, 1, 1, "", 2},
                 {"inputGain", "Input Gain", -12, 24, 0, "dB"}}) {}

void RoyLimiter::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    // Lookahead is fixed at prepare time (it defines the reported latency). +8 samples
    // cover the group delay of the true-peak estimator.
    lookahead_ = std::max(8, static_cast<int>(std::lround(p(Lookahead) * 0.001 * sr))) + 8;
    const size_t ring = static_cast<size_t>(lookahead_ + 1);
    delayL_.assign(ring, 0.0f);
    delayR_.assign(ring, 0.0f);
    peakBuf_.assign(ring * 2, 1.0f); // [0, ring): required gains, [ring, 2 ring): window minima
    osL_.prepare(maxBlock);
    osR_.prepare(maxBlock);
    reset();
}

void RoyLimiter::reset() {
    std::fill(delayL_.begin(), delayL_.end(), 0.0f);
    std::fill(delayR_.begin(), delayR_.end(), 0.0f);
    std::fill(peakBuf_.begin(), peakBuf_.end(), 1.0f);
    osL_.reset();
    osR_.reset();
    pos_ = 0;
    gain_ = 1.0;
}

void RoyLimiter::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    // Gain safety: gmin[k] = min(req[k-Lw .. k]); gs[n] = mean(gmin[n-Lw .. n]) <= req[n-Lw].
    // The audio is delayed by D samples and req refers to sample (n - detDelay), so with
    // Lw = D - detDelay every output sample gets a gain <= its required gain.
    const double sr = sampleRate_;
    const float ceiling = dbToGain(p(Ceiling));
    const float inGain = dbToGain(p(InputGain));
    const double rel = std::exp(-1.0 / (std::max(1.0f, p(Release)) * 0.001 * sr));
    const bool tp = p(TruePeak) > 0.5f;
    const int D = lookahead_;
    const int ring = D + 1;
    const int Lw = tp ? D - 8 : D;
    float* gReq = peakBuf_.data();
    float* gMin = peakBuf_.data() + ring;
    float* L = io.channel(0);
    float* R = io.channel(1);
    float maxGr = 0;
    for (int i = 0; i < io.numFrames; ++i) {
        const float xl = L[i] * inGain, xr = R[i] * inGain;
        float det = std::max(std::fabs(xl), std::fabs(xr));
        if (tp) {
            float up[4];
            osL_.up(&xl, up, 1);
            for (float u : up) det = std::max(det, std::fabs(u));
            osR_.up(&xr, up, 1);
            for (float u : up) det = std::max(det, std::fabs(u));
        }
        const int slot = pos_ % ring;
        gReq[slot] = det > ceiling ? ceiling / det : 1.0f;
        float m = 1.0f;
        for (int k = 0; k <= Lw; ++k) m = std::min(m, gReq[(slot - k + ring) % ring]);
        // moving average of the window minima over Lw + 1 samples (computed exactly)
        gMin[slot] = m;
        double acc = 0.0;
        for (int k = 0; k <= Lw; ++k) acc += gMin[(slot - k + ring) % ring];
        const double gs = std::min(1.0, acc / static_cast<double>(Lw + 1));
        gain_ = gs < gain_ ? gs : gs + (gain_ - gs) * rel;
        // delayed audio (x[n - D] sits in the slot that is overwritten next)
        const int rd = (pos_ + 1) % ring;
        const float dl = delayL_[static_cast<size_t>(rd)], dr = delayR_[static_cast<size_t>(rd)];
        delayL_[static_cast<size_t>(slot)] = xl;
        delayR_[static_cast<size_t>(slot)] = xr;
        const float g = static_cast<float>(gain_);
        L[i] = std::clamp(dl * g, -ceiling, ceiling); // final safety for sample peaks
        R[i] = std::clamp(dr * g, -ceiling, ceiling);
        maxGr = std::min(maxGr, static_cast<float>(gainToDb(static_cast<double>(g))));
        pos_ = (pos_ + 1) % (ring * 1024);
    }
    gr_.store(maxGr, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- gate
RoyGate::RoyGate()
    : Processor({{"threshold", "Threshold", -90, 0, -45, "dB"},
                 {"range", "Range", -90, 0, -40, "dB"},
                 {"attack", "Attack", 0.05f, 50, 0.5f, "ms"},
                 {"hold", "Hold", 0, 500, 30, "ms"},
                 {"release", "Release", 5, 2000, 120, "ms"},
                 {"sidechain", "External Sidechain", 0, 1, 0, "", 2}}) {}

void RoyGate::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoyGate::reset() {
    env_ = 0;
    gain_ = dbToGain(static_cast<double>(p(Range)));
    holdLeft_ = 0;
    open_ = false;
}

void RoyGate::process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const double thrOpen = dbToGain(static_cast<double>(p(Threshold)));
    const double thrClose = thrOpen * 0.708; // 3 dB hysteresis
    const double rangeG = dbToGain(static_cast<double>(p(Range)));
    const double att = std::exp(-1.0 / (std::max(0.05f, p(Attack)) * 0.001 * sr));
    const double rel = std::exp(-1.0 / (std::max(5.0f, p(Release)) * 0.001 * sr));
    const double detRel = std::exp(-1.0 / (0.02 * sr));
    const bool useSc = p(UseSidechain) > 0.5f && sc;
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int i = 0; i < io.numFrames; ++i) {
        const double x = useSc ? std::max(std::fabs(sc->channel(0)[i]), std::fabs(sc->channel(1)[i]))
                               : std::max(std::fabs(L[i]), std::fabs(R[i]));
        env_ = x > env_ ? x : x + (env_ - x) * detRel;
        if (env_ > thrOpen) {
            open_ = true;
            holdLeft_ = p(Hold) * 0.001 * sr;
        } else if (env_ < thrClose) {
            if (holdLeft_ > 0) holdLeft_ -= 1;
            else open_ = false;
        }
        const double target = open_ ? 1.0 : rangeG;
        gain_ = target > gain_ ? target + (gain_ - target) * att : target + (gain_ - target) * rel;
        L[i] = static_cast<float>(L[i] * gain_);
        R[i] = static_cast<float>(R[i] * gain_);
    }
}

// ---------------------------------------------------------------- de-esser
RoyDeEsser::RoyDeEsser()
    : Processor({{"frequency", "Frequency", 2000, 12000, 6500, "Hz"},
                 {"threshold", "Threshold", -60, 0, -28, "dB"},
                 {"range", "Max Reduction", 0, 24, 10, "dB"},
                 {"listen", "Listen (sibilance only)", 0, 1, 0, "", 2}}) {}

void RoyDeEsser::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    env_.set(sr, 0.0005, 0.03);
    lastFreq_ = -1;
    reset();
}

void RoyDeEsser::reset() {
    for (auto& f : shelf_) f.reset();
    for (auto& f : listenHp_) f.reset();
    det_.reset();
    env_.reset();
    gainDb_ = 0;
    counter_ = 0;
}

void RoyDeEsser::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const float f = p(Frequency);
    if (f != lastFreq_) {
        det_.set(dsp::Biquad::Type::HighPass, sr, f, 0.707);
        for (auto& h : listenHp_) h.set(dsp::Biquad::Type::HighPass, sr, f, 0.707);
        lastFreq_ = f;
        counter_ = 0;
    }
    const double thr = p(Threshold), range = p(Range);
    const bool listen = p(Listen) > 0.5f;
    const double gAtt = std::exp(-1.0 / (0.001 * sr)), gRel = std::exp(-1.0 / (0.06 * sr));
    float* L = io.channel(0);
    float* R = io.channel(1);
    float maxGr = 0;
    for (int i = 0; i < io.numFrames; ++i) {
        const float d = det_.process(0.5f * (L[i] + R[i]));
        const double lvl = gainToDb(static_cast<double>(env_.process(std::fabs(d))));
        const double target = std::max(-range, std::min(0.0, thr - lvl));
        gainDb_ = target < gainDb_ ? target + (gainDb_ - target) * gAtt : target + (gainDb_ - target) * gRel;
        if ((counter_++ & 15) == 0)
            for (auto& sh : shelf_) sh.set(dsp::Biquad::Type::HighShelf, sr, f, 0.707, gainDb_);
        if (listen) {
            L[i] = listenHp_[0].process(shelf_[0].process(L[i]));
            R[i] = listenHp_[1].process(shelf_[1].process(R[i]));
        } else {
            L[i] = shelf_[0].process(L[i]);
            R[i] = shelf_[1].process(R[i]);
        }
        maxGr = std::min(maxGr, static_cast<float>(gainDb_));
    }
    gr_.store(maxGr, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- transient shaper
RoyTransient::RoyTransient()
    : Processor({{"attack", "Attack", -100, 100, 0, "%"}, {"sustain", "Sustain", -100, 100, 0, "%"}, {"output", "Output", -24, 24, 0, "dB"}}) {}

void RoyTransient::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    fast_.set(sr, 0.0005, 0.05);
    slow_.set(sr, 0.03, 0.3);
    reset();
}

void RoyTransient::reset() {
    fast_.reset();
    slow_.reset();
}

void RoyTransient::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double a = p(AttackAmount) / 100.0, s = p(SustainAmount) / 100.0;
    const float out = dbToGain(p(Output));
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int i = 0; i < io.numFrames; ++i) {
        const float x = std::max(std::fabs(L[i]), std::fabs(R[i]));
        const double f = fast_.process(x) + 1e-9, sl = slow_.process(x) + 1e-9;
        const double d = 20.0 * std::log10(f / sl);
        const double gDb = std::clamp(d > 0 ? a * d * 2.0 : s * (-d) * 2.0, -18.0, 18.0);
        const float g = static_cast<float>(dbToGain(gDb)) * out;
        L[i] *= g;
        R[i] *= g;
    }
}

// ---------------------------------------------------------------- clipper
RoyClipper::RoyClipper()
    : Processor({{"ceiling", "Ceiling", -24, 0, -0.3f, "dB"}, {"softness", "Softness", 0, 1, 0.3f}, {"inputGain", "Input Gain", -12, 24, 0, "dB"}}) {}

void RoyClipper::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    for (auto& o : os_) o.prepare(maxBlock);
    hi_.assign(static_cast<size_t>(maxBlock * 4), 0.0f);
    reset();
}

void RoyClipper::reset() {
    for (auto& o : os_) o.reset();
}

void RoyClipper::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const float c = dbToGain(p(Ceiling)), s = p(Softness), in = dbToGain(p(InputGain));
    for (int ch = 0; ch < 2; ++ch) {
        float* x = io.channel(ch);
        for (int i = 0; i < io.numFrames; ++i) x[i] *= in;
        os_[ch].up(x, hi_.data(), io.numFrames);
        for (int i = 0; i < io.numFrames * 4; ++i) hi_[static_cast<size_t>(i)] = softClip(hi_[static_cast<size_t>(i)], c, s);
        os_[ch].down(hi_.data(), x, io.numFrames);
    }
}

} // namespace roy
