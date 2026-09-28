#include "effects/Special.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace roy {

// ================================================================ NoiseCleaner
RoyNoiseCleaner::RoyNoiseCleaner()
    : Processor({{"reduction", "Reduction", 0, 40, 12, "dB"},
                 {"sensitivity", "Sensitivity", 0.5f, 4, 2.0f},
                 {"learn", "Adaptive Learning", 0, 1, 1, "", 2},
                 {"smoothing", "Smoothing", 0, 0.99f, 0.7f}}) {}

void RoyNoiseCleaner::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    win_ = dsp::hannWindow(kN);
    for (auto& f : inFifo_) f.assign(kN, 0.0f);
    for (auto& f : outAcc_) f.assign(2 * kN, 0.0f);
    for (auto& f : outFifo_) f.assign(kHop, 0.0f);
    noise_.assign(kN / 2 + 1, 0.0f);
    gainSm_.assign(kN / 2 + 1, 1.0f);
    smoothP_.assign(kN / 2 + 1, 0.0f);
    rawGain_.assign(kN / 2 + 1, 1.0f);
    curMin_.assign(kN / 2 + 1, 1e30f);
    subMins_.assign(16, std::vector<float>(kN / 2 + 1, 1e30f)); // 16 x 32 frames ~ 2.7 s
    frame_.assign(kN, 0.0f);
    spec_.assign(kN / 2 + 1, dsp::cpx(0, 0));
    reset();
}

void RoyNoiseCleaner::reset() {
    for (auto& f : inFifo_) std::fill(f.begin(), f.end(), 0.0f);
    for (auto& f : outAcc_) std::fill(f.begin(), f.end(), 0.0f);
    for (auto& f : outFifo_) std::fill(f.begin(), f.end(), 0.0f);
    std::fill(noise_.begin(), noise_.end(), 0.0f);
    std::fill(gainSm_.begin(), gainSm_.end(), 1.0f);
    std::fill(smoothP_.begin(), smoothP_.end(), 0.0f);
    std::fill(curMin_.begin(), curMin_.end(), 1e30f);
    for (auto& m : subMins_) std::fill(m.begin(), m.end(), 1e30f);
    subIdx_ = frameInSub_ = 0;
    fifoPos_ = kN - kHop;
    frames_ = 0;
}

void RoyNoiseCleaner::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const int lat = kN - kHop;
    const float floorG = static_cast<float>(dbToGain(-static_cast<double>(p(Reduction))));
    const float sens = p(Sensitivity), smooth = p(Smoothing);
    const bool adaptive = p(Learn) > 0.5f;
    const size_t bins = static_cast<size_t>(kN / 2 + 1);
    for (int i = 0; i < io.numFrames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float* x = io.channel(ch);
            inFifo_[static_cast<size_t>(ch)][static_cast<size_t>(fifoPos_)] = x[i];
            x[i] = outFifo_[static_cast<size_t>(ch)][static_cast<size_t>(fifoPos_ - lat)];
        }
        if (++fifoPos_ < kN) continue;
        fifoPos_ = lat;
        // Power spectrum of both channels -> one shared gain curve.
        auto& specs = specs_;
        for (int ch = 0; ch < 2; ++ch) {
            for (int k = 0; k < kN; ++k) frame_[static_cast<size_t>(k)] = inFifo_[static_cast<size_t>(ch)][static_cast<size_t>(k)] * win_[static_cast<size_t>(k)];
            fft_.forwardReal(frame_.data(), specs[ch]);
        }
        ++frames_;
        const bool warm = frames_ > kN / kHop;          // the FIFO holds only real input now
        const bool track = adaptive || frames_ < 100;  // frozen profile: learn during the first ~0.5 s only
        if (warm && track && ++frameInSub_ >= 32) {    // close a minimum-statistics sub-window
            frameInSub_ = 0;
            subMins_[static_cast<size_t>(subIdx_)] = curMin_;
            subIdx_ = (subIdx_ + 1) % static_cast<int>(subMins_.size());
            std::fill(curMin_.begin(), curMin_.end(), 1e30f);
        }
        for (size_t b = 0; b < bins; ++b) {
            const float P = 0.5f * (std::norm(specs[0][b]) + std::norm(specs[1][b])) + 1e-20f;
            float g = 1.0f;
            if (warm) {
                smoothP_[b] = smoothP_[b] == 0.0f ? P : 0.8f * smoothP_[b] + 0.2f * P;
                if (track) curMin_[b] = std::min(curMin_[b], smoothP_[b]);
                float m = curMin_[b];
                for (auto& sm : subMins_) m = std::min(m, sm[b]);
                if (track || noise_[b] == 0.0f) noise_[b] = m >= 1e29f ? smoothP_[b] : 2.5f * m; // bias for the minimum
                g = std::max(floorG, 1.0f - sens * noise_[b] / P);
            }
            rawGain_[b] = g;
        }
        // Smooth gains across neighbouring bins and over time to suppress "musical noise".
        for (size_t b = 0; b < bins; ++b) {
            const float gl = rawGain_[b > 0 ? b - 1 : b], gr = rawGain_[b + 1 < bins ? b + 1 : b];
            const float g = 0.25f * gl + 0.5f * rawGain_[b] + 0.25f * gr;
            float& gs = gainSm_[b];
            gs = g > gs ? g : gs * smooth + g * (1.0f - smooth); // open instantly, close smoothly
        }
        for (int ch = 0; ch < 2; ++ch) {
            for (size_t b = 0; b < bins; ++b) spec_[b] = specs[ch][b] * gainSm_[b];
            fft_.inverseReal(spec_.data(), frame_.data());
            auto& acc = outAcc_[static_cast<size_t>(ch)];
            for (int k = 0; k < kN; ++k) acc[static_cast<size_t>(k)] += frame_[static_cast<size_t>(k)] * win_[static_cast<size_t>(k)] * (1.0f / 1.5f);
            // [0, hop) is complete now: it is emitted during the next hop
            std::memcpy(outFifo_[static_cast<size_t>(ch)].data(), acc.data(), sizeof(float) * kHop);
            std::memmove(acc.data(), acc.data() + kHop, sizeof(float) * static_cast<size_t>(2 * kN - kHop));
            std::memset(acc.data() + 2 * kN - kHop, 0, sizeof(float) * kHop);
            auto& in = inFifo_[static_cast<size_t>(ch)];
            std::memmove(in.data(), in.data() + kHop, sizeof(float) * static_cast<size_t>(kN - kHop));
        }
    }
}

// ================================================================ Analyzer
RoyAnalyzer::RoyAnalyzer() : Processor({}) {
    for (auto& b : bands_) b.store(-120.0f);
}

void RoyAnalyzer::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    win_ = dsp::hannWindow(kN);
    ring_.assign(kN, 0.0f);
    frame_.assign(kN, 0.0f);
    spec_.assign(kN / 2 + 1, dsp::cpx(0, 0));
    meter_.prepare(sr, maxBlock);
    reset();
}

void RoyAnalyzer::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    ringPos_ = sinceFft_ = 0;
    sLR_ = sLL_ = sRR_ = 0;
    meter_.reset();
}

double RoyAnalyzer::bandCentreHz(int band) const { return 20.0 * std::pow(1000.0, (band + 0.5) / kBands); }

void RoyAnalyzer::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const float* L = io.channel(0);
    const float* R = io.channel(1);
    meter_.process(L, R, io.numFrames);
    const double decay = std::exp(-static_cast<double>(io.numFrames) / (0.3 * sampleRate_));
    double lr = 0, ll = 0, rr = 0;
    for (int i = 0; i < io.numFrames; ++i) {
        ring_[static_cast<size_t>(ringPos_)] = 0.5f * (L[i] + R[i]);
        ringPos_ = (ringPos_ + 1) % kN;
        lr += static_cast<double>(L[i]) * R[i];
        ll += static_cast<double>(L[i]) * L[i];
        rr += static_cast<double>(R[i]) * R[i];
    }
    sLR_ = sLR_ * decay + lr;
    sLL_ = sLL_ * decay + ll;
    sRR_ = sRR_ * decay + rr;
    corr_.store(sLL_ > 1e-12 && sRR_ > 1e-12 ? static_cast<float>(sLR_ / std::sqrt(sLL_ * sRR_)) : 1.0f, std::memory_order_relaxed);
    sinceFft_ += io.numFrames;
    if (sinceFft_ < 1024) return;
    sinceFft_ = 0;
    for (int k = 0; k < kN; ++k) frame_[static_cast<size_t>(k)] = ring_[static_cast<size_t>((ringPos_ + k) % kN)] * win_[static_cast<size_t>(k)];
    fft_.forwardReal(frame_.data(), spec_.data());
    const double binHz = sampleRate_ / kN;
    for (int b = 0; b < kBands; ++b) {
        const double lo = 20.0 * std::pow(1000.0, static_cast<double>(b) / kBands);
        const double hi = 20.0 * std::pow(1000.0, static_cast<double>(b + 1) / kBands);
        int k0 = static_cast<int>(lo / binHz), k1 = std::max(k0 + 1, static_cast<int>(hi / binHz));
        double e = 0;
        for (int k = k0; k < k1 && k <= kN / 2; ++k) e = std::max(e, static_cast<double>(std::norm(spec_[static_cast<size_t>(k)])));
        const float db = static_cast<float>(10.0 * std::log10(e / (kN * 0.25 * kN * 0.25) + 1e-20));
        bands_[static_cast<size_t>(b)].store(db, std::memory_order_relaxed);
    }
}

// ================================================================ realtime pitch shifter
void PitchShifterRt::prepare(double sr, double windowMs) {
    sr_ = sr;
    window_ = std::max(64.0, windowMs * 0.001 * sr);
    buf_.assign(static_cast<size_t>(window_ * 2 + 16), 0.0f);
    reset();
}

void PitchShifterRt::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
}

float PitchShifterRt::process(float x, double ratio) noexcept {
    const int size = static_cast<int>(buf_.size());
    buf_[static_cast<size_t>(pos_)] = x;
    phase_ += (1.0 - ratio) / window_;
    phase_ -= std::floor(phase_);
    auto tap = [&](double ph) {
        const double d = 2.0 + ph * window_;
        double rp = pos_ - d;
        while (rp < 0) rp += size;
        const int i0 = static_cast<int>(rp);
        const float f = static_cast<float>(rp - i0);
        return buf_[static_cast<size_t>(i0)] * (1 - f) + buf_[static_cast<size_t>((i0 + 1) % size)] * f;
    };
    const double p2 = phase_ + 0.5 - std::floor(phase_ + 0.5);
    const float w1 = static_cast<float>(std::sin(kPi * phase_)), w2 = static_cast<float>(std::sin(kPi * p2));
    const float y = tap(phase_) * w1 * w1 + tap(p2) * w2 * w2;
    pos_ = (pos_ + 1) % size;
    return y;
}

RoyPitch::RoyPitch()
    : Processor({{"semitones", "Semitones", -24, 24, 0, "st"},
                 {"cents", "Fine", -100, 100, 0, "cents"},
                 {"mix", "Mix", 0, 1, 1},
                 {"window", "Window", 10, 100, 40, "ms"}}) {}

void RoyPitch::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    for (auto& s : sh_) s.prepare(sr, p(Window));
}

void RoyPitch::reset() {
    for (auto& s : sh_) s.reset();
}

void RoyPitch::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double ratio = std::pow(2.0, (p(Semitones) + p(Cents) / 100.0) / 12.0);
    const float mix = p(Mix);
    for (int ch = 0; ch < 2; ++ch) {
        float* x = io.channel(ch);
        for (int i = 0; i < io.numFrames; ++i) x[i] = x[i] * (1 - mix) + sh_[ch].process(x[i], ratio) * mix;
    }
}

// ================================================================ VocalTune (realtime)
RoyVocalTune::RoyVocalTune()
    : Processor({{"keyRoot", "Key Root", 0, 11, 0, "", 12},
                 {"keyScale", "Key Scale", 0, 12, 1, "", 13},
                 {"speed", "Retune Speed", 0, 400, 40, "ms"},
                 {"strength", "Strength", 0, 1, 1},
                 {"offKeyFilter", "Off-Key Filter", 0, 1, 1, "", 2},
                 {"allowChromatic", "Allow Chromatic", 0, 1, 0, "", 2},
                 {"threshold", "Threshold", 0, 50, 15, "cents"}}) {}

void RoyVocalTune::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    for (auto& s : sh_) s.prepare(sr, 30.0);
    decim_ = std::max(1, static_cast<int>(std::lround(sr / 24000.0)));
    ring_.assign(1024, 0.0f);
    frame_.assign(512 + 400, 0.0f);
    diff_.assign(400, 0.0f);
    reset();
}

void RoyVocalTune::reset() {
    for (auto& s : sh_) s.reset();
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    ringPos_ = sinceAnalysis_ = 0;
    decimAcc_ = 0;
    decimCount_ = 0;
    targetShift_ = shift_ = 0;
}

void RoyVocalTune::analyse() noexcept {
    const double srd = sampleRate_ / decim_;
    const int W = 512;
    const int maxLag = std::min(static_cast<int>(diff_.size()) - 1, static_cast<int>(srd / 70.0));
    const int minLag = std::max(2, static_cast<int>(srd / 1000.0));
    const int total = W + maxLag;
    const int rs = static_cast<int>(ring_.size());
    for (int k = 0; k < total; ++k) frame_[static_cast<size_t>(k)] = ring_[static_cast<size_t>((ringPos_ - total + k + rs * 4) % rs)];
    double e = 0;
    for (int k = 0; k < W; ++k) e += static_cast<double>(frame_[static_cast<size_t>(k)]) * frame_[static_cast<size_t>(k)];
    if (e / W < 1e-6) { // silence / unvoiced -> no correction
        targetShift_ = 0;
        detected_.store(0.0, std::memory_order_relaxed);
        return;
    }
    double running = 0;
    int best = -1;
    diff_[0] = 1.0f;
    for (int tau = 1; tau <= maxLag; ++tau) {
        double d = 0;
        for (int k = 0; k < W; ++k) {
            const double q = frame_[static_cast<size_t>(k)] - frame_[static_cast<size_t>(k + tau)];
            d += q * q;
        }
        running += d;
        diff_[static_cast<size_t>(tau)] = static_cast<float>(running > 0 ? d * tau / running : 1.0);
    }
    for (int tau = minLag; tau < maxLag; ++tau)
        if (diff_[static_cast<size_t>(tau)] < 0.15f) {
            while (tau + 1 < maxLag && diff_[static_cast<size_t>(tau + 1)] < diff_[static_cast<size_t>(tau)]) ++tau;
            best = tau;
            break;
        }
    if (best < 0) {
        targetShift_ = 0;
        detected_.store(0.0, std::memory_order_relaxed);
        return;
    }
    double tauF = best;
    const double y0 = diff_[static_cast<size_t>(best - 1)], y1 = diff_[static_cast<size_t>(best)], y2 = diff_[static_cast<size_t>(best + 1)];
    const double den = y0 - 2 * y1 + y2;
    if (std::fabs(den) > 1e-12) tauF += 0.5 * (y0 - y2) / den;
    const double m = hzToMidi(srd / tauF);
    detected_.store(m, std::memory_order_relaxed);
    const Key key{static_cast<int>(p(KeyRoot)), scaleTypeByIndex(static_cast<int>(p(KeyScale)))};
    const int semi = static_cast<int>(std::lround(m));
    double target = semi;
    if (!key.contains(semi) && p(OffKeyFilter) > 0.5f && p(AllowChromatic) < 0.5f) target = key.nearestPitch(m);
    if (std::fabs(m - target) * 100.0 <= p(Threshold)) target = m; // within threshold: leave natural
    targetShift_ = (target - m) * p(Strength);
}

void RoyVocalTune::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    float* L = io.channel(0);
    float* R = io.channel(1);
    const double speed = p(Speed);
    const double coef = speed <= 0 ? 0.0 : std::exp(-1.0 / (speed * 0.001 * sampleRate_));
    const int rs = static_cast<int>(ring_.size());
    for (int i = 0; i < io.numFrames; ++i) {
        decimAcc_ += 0.5f * (L[i] + R[i]);
        if (++decimCount_ >= decim_) {
            ring_[static_cast<size_t>(ringPos_)] = decimAcc_ / static_cast<float>(decim_);
            ringPos_ = (ringPos_ + 1) % rs;
            decimAcc_ = 0;
            decimCount_ = 0;
            if (++sinceAnalysis_ >= 128) {
                sinceAnalysis_ = 0;
                analyse();
            }
        }
        shift_ = targetShift_ + (shift_ - targetShift_) * coef;
        const double ratio = std::pow(2.0, shift_ / 12.0);
        L[i] = sh_[0].process(L[i], ratio);
        R[i] = sh_[1].process(R[i], ratio);
    }
    shiftOut_.store(shift_, std::memory_order_relaxed);
}

// ================================================================ Dynamic Space
RoyDynamicSpace::RoyDynamicSpace()
    : Processor({{"frequency", "Frequency", 60, 12000, 2500, "Hz"},
                 {"bandwidth", "Bandwidth", 0.2f, 3, 1.0f, "oct"},
                 {"maxReduction", "Max Reduction", -24, 0, -6, "dB"},
                 {"attack", "Attack", 0.5f, 200, 10, "ms"},
                 {"release", "Release", 10, 2000, 150, "ms"},
                 {"sensitivity", "Threshold", -60, 0, -30, "dB"}}) {}

void RoyDynamicSpace::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    lastFreq_ = lastBw_ = -1;
    reset();
}

void RoyDynamicSpace::reset() {
    for (auto& f : detect_) f.reset();
    for (auto& f : eq_) f.reset();
    env_ = 0;
    gainDb_ = 0;
    counter_ = 0;
}

void RoyDynamicSpace::process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const float f = p(Frequency), bw = p(Bandwidth);
    const double q = std::sqrt(std::pow(2.0, bw)) / (std::pow(2.0, bw) - 1.0);
    if (f != lastFreq_ || bw != lastBw_) {
        for (auto& d : detect_) d.set(dsp::Biquad::Type::BandPass, sr, f, q);
        lastFreq_ = f;
        lastBw_ = bw;
        counter_ = 0;
    }
    const double att = std::exp(-1.0 / (p(Attack) * 0.001 * sr)), rel = std::exp(-1.0 / (p(Release) * 0.001 * sr));
    const double detAtt = std::exp(-1.0 / (0.001 * sr)), detRel = std::exp(-1.0 / (0.02 * sr)); // level detector
    const double thr = p(Sensitivity), maxRed = p(MaxReduction);
    float* L = io.channel(0);
    float* R = io.channel(1);
    const float* kL = sc ? sc->channel(0) : L;
    const float* kR = sc ? sc->channel(1) : R;
    float minRed = 0;
    for (int i = 0; i < io.numFrames; ++i) {
        const float k = 0.5f * (detect_[0].process(kL[i]) + detect_[1].process(kR[i]));
        const double a = std::fabs(k);
        env_ = a > env_ ? a + (env_ - a) * detAtt : a + (env_ - a) * detRel;
        const double lvl = gainToDb(env_ * 1.4142); // sine peak -> level
        const double target = std::clamp(-(lvl - thr), maxRed, 0.0); // 1 dB cut per dB above threshold
        gainDb_ = target < gainDb_ ? target + (gainDb_ - target) * att : target + (gainDb_ - target) * rel;
        if ((counter_++ & 15) == 0)
            for (auto& e : eq_) e.set(dsp::Biquad::Type::Peak, sr, f, q, gainDb_);
        L[i] = eq_[0].process(L[i]);
        R[i] = eq_[1].process(R[i]);
        minRed = std::min(minRed, static_cast<float>(gainDb_));
    }
    red_.store(minRed, std::memory_order_relaxed);
}

} // namespace roy
