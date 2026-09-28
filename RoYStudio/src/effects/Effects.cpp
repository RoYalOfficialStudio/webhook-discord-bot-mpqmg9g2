#include "effects/Effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace roy {

// ================================================================ EQ
RoyEq::RoyEq()
    : Processor({{"lowCut", "Low Cut", 20, 1000, 20, "Hz"},
                 {"lowCutSlope", "Low Cut Slope", 12, 24, 12, "dB/oct", 2},
                 {"highCut", "High Cut", 1000, 20000, 20000, "Hz"},
                 {"band1Freq", "Low Shelf Freq", 20, 1000, 100, "Hz"},
                 {"band1Gain", "Low Shelf Gain", -24, 24, 0, "dB"},
                 {"band1Q", "Low Shelf Q", 0.3f, 2, 0.707f},
                 {"band2Freq", "Low Mid Freq", 40, 8000, 400, "Hz"},
                 {"band2Gain", "Low Mid Gain", -24, 24, 0, "dB"},
                 {"band2Q", "Low Mid Q", 0.1f, 18, 1},
                 {"band3Freq", "High Mid Freq", 200, 16000, 3000, "Hz"},
                 {"band3Gain", "High Mid Gain", -24, 24, 0, "dB"},
                 {"band3Q", "High Mid Q", 0.1f, 18, 1},
                 {"band4Freq", "High Shelf Freq", 1000, 20000, 10000, "Hz"},
                 {"band4Gain", "High Shelf Gain", -24, 24, 0, "dB"},
                 {"band4Q", "High Shelf Q", 0.3f, 2, 0.707f},
                 {"output", "Output", -24, 24, 0, "dB"}}) {}

void RoyEq::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    cache_.fill(-1e9f);
    update();
    reset();
}

void RoyEq::reset() {
    for (auto& c : lc_)
        for (auto& f : c) f.reset();
    for (auto& f : hc_) f.reset();
    for (auto& c : bands_)
        for (auto& f : c) f.reset();
}

void RoyEq::update() noexcept {
    bool changed = false;
    for (int i = 0; i < NumParams; ++i)
        if (cache_[static_cast<size_t>(i)] != p(i)) {
            cache_[static_cast<size_t>(i)] = p(i);
            changed = true;
        }
    if (!changed) return;
    const double sr = sampleRate_;
    lcOn_ = p(LowCut) > 20.5f;
    lcStages_ = p(LowCutSlope) > 18 ? 2 : 1;
    hcOn_ = p(HighCut) < 19999.0f && p(HighCut) < sr * 0.45;
    for (int ch = 0; ch < 2; ++ch) {
        // Butterworth sections: 12 dB -> Q 0.707; 24 dB -> Q 0.541 and 1.307
        if (lcStages_ == 1) lc_[ch][0].set(dsp::Biquad::Type::HighPass, sr, p(LowCut), 0.7071);
        else {
            lc_[ch][0].set(dsp::Biquad::Type::HighPass, sr, p(LowCut), 0.5412);
            lc_[ch][1].set(dsp::Biquad::Type::HighPass, sr, p(LowCut), 1.3066);
        }
        hc_[ch].set(dsp::Biquad::Type::LowPass, sr, p(HighCut), 0.7071);
        bands_[ch][0].set(dsp::Biquad::Type::LowShelf, sr, p(B1Freq), p(B1Q), p(B1Gain));
        bands_[ch][1].set(dsp::Biquad::Type::Peak, sr, p(B2Freq), p(B2Q), p(B2Gain));
        bands_[ch][2].set(dsp::Biquad::Type::Peak, sr, p(B3Freq), p(B3Q), p(B3Gain));
        bands_[ch][3].set(dsp::Biquad::Type::HighShelf, sr, p(B4Freq), p(B4Q), p(B4Gain));
    }
}

double RoyEq::responseDb(double hz) const {
    const double sr = sampleRate_;
    double m = 1.0;
    if (lcOn_) m *= lc_[0][0].magnitudeAt(hz, sr) * (lcStages_ == 2 ? lc_[0][1].magnitudeAt(hz, sr) : 1.0);
    if (hcOn_) m *= hc_[0].magnitudeAt(hz, sr);
    for (int b = 0; b < 4; ++b)
        if (std::fabs(p(B1Gain + b * 3)) > 1e-4f) m *= bands_[0][b].magnitudeAt(hz, sr);
    return gainToDb(m) + p(Output);
}

void RoyEq::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    update();
    const float out = dbToGain(p(Output));
    bool bandOn[4];
    for (int b = 0; b < 4; ++b) bandOn[b] = std::fabs(p(B1Gain + b * 3)) > 1e-4f;
    for (int ch = 0; ch < 2; ++ch) {
        float* x = io.channel(ch);
        for (int i = 0; i < io.numFrames; ++i) {
            float v = x[i];
            if (lcOn_) {
                v = lc_[ch][0].process(v);
                if (lcStages_ == 2) v = lc_[ch][1].process(v);
            }
            for (int b = 0; b < 4; ++b)
                if (bandOn[b]) v = bands_[ch][b].process(v);
            if (hcOn_) v = hc_[ch].process(v);
            x[i] = v * out;
        }
    }
}

// ================================================================ Reverb
RoyReverb::RoyReverb()
    : Processor({{"decay", "Decay (RT60)", 0.1f, 20, 2.0f, "s"},
                 {"preDelay", "Pre-Delay", 0, 250, 20, "ms"},
                 {"size", "Size", 0.2f, 2.0f, 1.0f},
                 {"damping", "HF Damping", 0, 1, 0.4f},
                 {"width", "Width", 0, 1, 1},
                 {"mix", "Mix", 0, 1, 0.25f},
                 {"lowCut", "Input Low Cut", 20, 1000, 150, "Hz"}}) {}

void RoyReverb::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    static const int base[kLines] = {1433, 1601, 1867, 2053, 2251, 2399, 2687, 2903}; // ~30-60 ms @48k, mutually prime
    for (int i = 0; i < kLines; ++i) {
        const int maxLen = static_cast<int>(base[i] * sr / 48000.0 * 2.1) + 4;
        lines_[static_cast<size_t>(i)].assign(static_cast<size_t>(maxLen), 0.0f);
    }
    pre_.assign(static_cast<size_t>(0.26 * sr) + 4, 0.0f);
    lastSize_ = -1;
    reset();
}

void RoyReverb::reset() {
    for (auto& l : lines_) std::fill(l.begin(), l.end(), 0.0f);
    pos_.fill(0);
    lp_.fill(0.0f);
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    prePos_ = 0;
    inHp_.reset();
}

void RoyReverb::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    static const int base[kLines] = {1433, 1601, 1867, 2053, 2251, 2399, 2687, 2903};
    const float size = p(Size);
    if (size != lastSize_) {
        for (int i = 0; i < kLines; ++i) {
            len_[static_cast<size_t>(i)] = std::clamp(static_cast<int>(base[i] * sr / 48000.0 * size), 16,
                                                      static_cast<int>(lines_[static_cast<size_t>(i)].size()) - 1);
            pos_[static_cast<size_t>(i)] %= len_[static_cast<size_t>(i)];
        }
        lastSize_ = size;
    }
    inHp_.set(dsp::Biquad::Type::HighPass, sr, p(LowCut), 0.707);
    const double rt60 = std::max(0.1f, p(Decay));
    float g[kLines];
    for (int i = 0; i < kLines; ++i) g[i] = static_cast<float>(std::pow(10.0, -3.0 * len_[static_cast<size_t>(i)] / (rt60 * sr)));
    const float damp = p(Damping) * 0.85f;
    const float mix = p(Mix), width = p(Width);
    const int pre = std::min(static_cast<int>(pre_.size()) - 2, static_cast<int>(p(PreDelay) * 0.001 * sr));
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int n = 0; n < io.numFrames; ++n) {
        const float in = inHp_.process(0.5f * (L[n] + R[n]));
        pre_[static_cast<size_t>(prePos_)] = in;
        const int rp = (prePos_ - pre + static_cast<int>(pre_.size())) % static_cast<int>(pre_.size());
        const float x = pre_[static_cast<size_t>(rp)];
        prePos_ = (prePos_ + 1) % static_cast<int>(pre_.size());
        float o[kLines];
        float sum = 0;
        for (int i = 0; i < kLines; ++i) {
            const auto& line = lines_[static_cast<size_t>(i)];
            float v = line[static_cast<size_t>(pos_[static_cast<size_t>(i)])];
            lp_[static_cast<size_t>(i)] = v + (lp_[static_cast<size_t>(i)] - v) * damp; // one-pole HF damping
            o[i] = lp_[static_cast<size_t>(i)] * g[i];
            sum += o[i];
        }
        const float h = sum * (2.0f / kLines); // Householder: y = x - 2/N * sum
        for (int i = 0; i < kLines; ++i) {
            auto& line = lines_[static_cast<size_t>(i)];
            const float fbv = o[i] - h + x * ((i & 1) ? -0.35f : 0.35f);
            line[static_cast<size_t>(pos_[static_cast<size_t>(i)])] = sanitize(fbv);
            pos_[static_cast<size_t>(i)] = (pos_[static_cast<size_t>(i)] + 1) % len_[static_cast<size_t>(i)];
        }
        const float wl = o[0] + o[2] + o[4] + o[6];
        const float wr = o[1] + o[3] + o[5] + o[7];
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
        L[n] = L[n] * (1.0f - mix) + (mid + side) * mix;
        R[n] = R[n] * (1.0f - mix) + (mid - side) * mix;
    }
}

// ================================================================ Delay
RoyDelay::RoyDelay()
    : Processor({{"time", "Time", 1, 2000, 375, "ms"},
                 {"syncBeats", "Sync (beats, 0 = off)", 0, 4, 0, "beats"},
                 {"feedback", "Feedback", 0, 0.95f, 0.35f},
                 {"mix", "Mix", 0, 1, 0.25f},
                 {"pingPong", "Ping-Pong", 0, 1, 0, "", 2},
                 {"lowCut", "Feedback Low Cut", 20, 2000, 150, "Hz"},
                 {"highCut", "Feedback High Cut", 1000, 20000, 8000, "Hz"}}) {}

void RoyDelay::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    const size_t len = static_cast<size_t>(sr * 4.1) + 4; // up to 4 s (or 4 beats at >= 60 bpm)
    bufL_.assign(len, 0.0f);
    bufR_.assign(len, 0.0f);
    time_.setTime(sr, 0.05);
    time_.reset(static_cast<float>(currentDelaySeconds() * sr));
    lastLc_ = lastHc_ = -1;
    reset();
}

void RoyDelay::reset() {
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    pos_ = 0;
    for (auto& f : lc_) f.reset();
    for (auto& f : hc_) f.reset();
}

double RoyDelay::currentDelaySeconds() const {
    const double beats = p(SyncBeats);
    if (beats > 0) return beats * 60.0 / std::max(20.0, bpm_.load());
    return p(TimeMs) * 0.001;
}

void RoyDelay::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    if (p(LowCut) != lastLc_ || p(HighCut) != lastHc_) {
        for (auto& f : lc_) f.set(dsp::Biquad::Type::HighPass, sr, p(LowCut), 0.707);
        for (auto& f : hc_) f.set(dsp::Biquad::Type::LowPass, sr, p(HighCut), 0.707);
        lastLc_ = p(LowCut);
        lastHc_ = p(HighCut);
    }
    const int size = static_cast<int>(bufL_.size());
    const float targetSamples = static_cast<float>(std::clamp(currentDelaySeconds() * sr, 1.0, static_cast<double>(size - 3)));
    const float fb = p(Feedback), mix = p(Mix);
    const bool pp = p(PingPong) > 0.5f;
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int i = 0; i < io.numFrames; ++i) {
        const float d = time_.next(targetSamples);
        double rp = pos_ - d;
        while (rp < 0) rp += size;
        const int i0 = static_cast<int>(rp);
        const float f = static_cast<float>(rp - i0);
        const int i1 = (i0 + 1) % size;
        const float dl = bufL_[static_cast<size_t>(i0)] * (1 - f) + bufL_[static_cast<size_t>(i1)] * f;
        const float dr = bufR_[static_cast<size_t>(i0)] * (1 - f) + bufR_[static_cast<size_t>(i1)] * f;
        const float fl = hc_[0].process(lc_[0].process(dl)) * fb;
        const float fr = hc_[1].process(lc_[1].process(dr)) * fb;
        if (pp) {
            bufL_[static_cast<size_t>(pos_)] = sanitize(0.5f * (L[i] + R[i]) + fr);
            bufR_[static_cast<size_t>(pos_)] = sanitize(fl);
        } else {
            bufL_[static_cast<size_t>(pos_)] = sanitize(L[i] + fl);
            bufR_[static_cast<size_t>(pos_)] = sanitize(R[i] + fr);
        }
        pos_ = (pos_ + 1) % size;
        L[i] = L[i] * (1 - mix) + dl * mix;
        R[i] = R[i] * (1 - mix) + dr * mix;
    }
}

// ================================================================ Chorus / Flanger
RoyModDelay::RoyModDelay(bool flanger)
    : Processor(flanger ? std::vector<ParamInfo>{{"rate", "Rate", 0.01f, 5, 0.25f, "Hz"},
                                                 {"depth", "Depth", 0, 10, 2.0f, "ms"},
                                                 {"delay", "Delay", 0.1f, 10, 1.0f, "ms"},
                                                 {"feedback", "Feedback", -0.95f, 0.95f, 0.5f},
                                                 {"mix", "Mix", 0, 1, 0.5f},
                                                 {"spread", "Stereo Spread", 0, 1, 0.5f}}
                        : std::vector<ParamInfo>{{"rate", "Rate", 0.01f, 5, 0.8f, "Hz"},
                                                 {"depth", "Depth", 0, 10, 3.0f, "ms"},
                                                 {"delay", "Delay", 5, 40, 15.0f, "ms"},
                                                 {"feedback", "Feedback", -0.95f, 0.95f, 0.0f},
                                                 {"mix", "Mix", 0, 1, 0.4f},
                                                 {"spread", "Stereo Spread", 0, 1, 1.0f}}),
      flanger_(flanger) {}

void RoyModDelay::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    for (auto& b : buf_) b.assign(static_cast<size_t>(0.06 * sr) + 8, 0.0f);
    reset();
}

void RoyModDelay::reset() {
    for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
    fb_[0] = fb_[1] = 0;
}

void RoyModDelay::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const int size = static_cast<int>(buf_[0].size());
    const double rate = p(Rate), depth = p(Depth) * 0.001 * sr, base = p(Delay) * 0.001 * sr;
    const float fbk = p(Feedback), mix = p(Mix);
    const double spread = p(Spread) * 0.5;
    for (int i = 0; i < io.numFrames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float* x = io.channel(ch);
            const double lfo = 0.5 + 0.5 * std::sin(kTwoPi * (phase_ + (ch ? spread : 0.0)));
            const double d = std::clamp(base + depth * lfo, 1.0, static_cast<double>(size - 3));
            double rp = pos_ - d;
            while (rp < 0) rp += size;
            const int i0 = static_cast<int>(rp);
            const float f = static_cast<float>(rp - i0);
            auto& b = buf_[ch];
            const float y = b[static_cast<size_t>(i0)] * (1 - f) + b[static_cast<size_t>((i0 + 1) % size)] * f;
            b[static_cast<size_t>(pos_)] = sanitize(x[i] + y * fbk);
            x[i] = x[i] * (1 - mix) + y * mix;
        }
        pos_ = (pos_ + 1) % size;
        phase_ += rate / sr;
        if (phase_ >= 1) phase_ -= 1;
    }
}

// ================================================================ Phaser
RoyPhaser::RoyPhaser()
    : Processor({{"rate", "Rate", 0.01f, 5, 0.3f, "Hz"},
                 {"depth", "Depth", 0, 1, 0.8f},
                 {"stages", "Stages", 2, 12, 6, "", 11},
                 {"feedback", "Feedback", -0.9f, 0.9f, 0.4f},
                 {"mix", "Mix", 0, 1, 0.5f},
                 {"centre", "Centre", 100, 4000, 800, "Hz"}}) {}

void RoyPhaser::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoyPhaser::reset() {
    for (auto& z : z_) z.fill(0.0f);
    phase_ = 0;
    fb_[0] = fb_[1] = 0;
}

void RoyPhaser::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    const double sr = sampleRate_;
    const int stages = std::clamp(static_cast<int>(p(Stages)), 2, 12);
    const float fbk = p(Feedback), mix = p(Mix);
    const double depth = p(Depth), centre = p(Centre), rate = p(Rate);
    for (int i = 0; i < io.numFrames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float* x = io.channel(ch);
            const double lfo = std::sin(kTwoPi * (phase_ + ch * 0.25));
            const double f = std::clamp(centre * std::pow(2.0, 2.0 * depth * lfo), 20.0, sr * 0.45);
            const double t = std::tan(kPi * f / sr);
            const float a = static_cast<float>((t - 1.0) / (t + 1.0)); // first-order all-pass coefficient
            float v = x[i] + fb_[ch] * fbk;
            for (int s = 0; s < stages; ++s) {
                const float y = a * v + z_[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                z_[static_cast<size_t>(ch)][static_cast<size_t>(s)] = sanitize(v - a * y);
                v = y;
            }
            fb_[ch] = sanitize(v);
            x[i] = x[i] * (1 - mix * 0.5f) + v * mix * 0.5f;
        }
        phase_ += rate / sr;
        if (phase_ >= 1) phase_ -= 1;
    }
}

// ================================================================ Stereo
RoyStereo::RoyStereo()
    : Processor({{"width", "Width", 0, 2, 1},
                 {"monoBass", "Mono Below", 20, 500, 20, "Hz"},
                 {"balance", "Balance", -1, 1, 0},
                 {"midGain", "Mid Gain", -24, 12, 0, "dB"},
                 {"sideGain", "Side Gain", -24, 12, 0, "dB"}}) {}

void RoyStereo::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    lastMono_ = -1;
    reset();
}

void RoyStereo::reset() {
    sideHp_.reset();
    sideHp2_.reset();
}

void RoyStereo::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    if (p(MonoBass) != lastMono_) {
        sideHp_.set(dsp::Biquad::Type::HighPass, sampleRate_, p(MonoBass), 0.5412); // 24 dB/oct Butterworth
        sideHp2_.set(dsp::Biquad::Type::HighPass, sampleRate_, p(MonoBass), 1.3066);
        lastMono_ = p(MonoBass);
    }
    const bool monoBass = p(MonoBass) > 20.5f;
    const float w = p(Width), mg = dbToGain(p(MidGain)), sg = dbToGain(p(SideGain));
    const float bal = p(Balance);
    const float bl = bal > 0 ? 1.0f - bal : 1.0f, br = bal < 0 ? 1.0f + bal : 1.0f;
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int i = 0; i < io.numFrames; ++i) {
        const float m = 0.5f * (L[i] + R[i]) * mg;
        float s = 0.5f * (L[i] - R[i]) * sg * w;
        if (monoBass) s = sideHp2_.process(sideHp_.process(s));
        L[i] = (m + s) * bl;
        R[i] = (m - s) * br;
    }
}

// ================================================================ Saturation / Distortion
RoySaturation::RoySaturation(bool distortion)
    : Processor(distortion ? std::vector<ParamInfo>{{"drive", "Drive", 0, 48, 18, "dB"},
                                                    {"mode", "Mode (0 fuzz, 1 overdrive, 2 rectify)", 0, 2, 1, "", 3},
                                                    {"tone", "Tone", 500, 20000, 6000, "Hz"},
                                                    {"mix", "Mix", 0, 1, 1},
                                                    {"output", "Output", -48, 12, -12, "dB"}}
                           : std::vector<ParamInfo>{{"drive", "Drive", 0, 24, 6, "dB"},
                                                    {"mode", "Mode (0 tape, 1 tube, 2 soft)", 0, 2, 0, "", 3},
                                                    {"tone", "Tone", 1000, 20000, 16000, "Hz"},
                                                    {"mix", "Mix", 0, 1, 1},
                                                    {"output", "Output", -24, 12, -3, "dB"}}),
      distortion_(distortion) {}

void RoySaturation::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    for (auto& o : os_) o.prepare(maxBlock);
    hi_.assign(static_cast<size_t>(maxBlock * 4), 0.0f);
    wet_.assign(static_cast<size_t>(maxBlock), 0.0f);
    for (auto& l : dryLine_) l.assign(static_cast<size_t>(std::max(1, os_[0].latencySamples())), 0.0f); // read-then-write ring = exact delay
    for (auto& d : dc_) d.set(sr, 10.0);
    lastTone_ = -1;
    reset();
}

void RoySaturation::reset() {
    for (auto& o : os_) o.reset();
    for (auto& l : dryLine_) std::fill(l.begin(), l.end(), 0.0f);
    dryPos_[0] = dryPos_[1] = 0;
    for (auto& t : tone_) t.reset();
    for (auto& d : dc_) d.reset();
}

void RoySaturation::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    if (p(Tone) != lastTone_) {
        for (auto& t : tone_) t.set(dsp::Biquad::Type::LowPass, sampleRate_, p(Tone), 0.707);
        lastTone_ = p(Tone);
    }
    const float drive = dbToGain(p(Drive)), out = dbToGain(p(Output)), mix = p(Mix);
    const int mode = static_cast<int>(p(Mode));
    for (int ch = 0; ch < 2; ++ch) {
        float* x = io.channel(ch);
        os_[ch].up(x, hi_.data(), io.numFrames);
        for (int i = 0; i < io.numFrames * 4; ++i) {
            float v = hi_[static_cast<size_t>(i)] * drive;
            if (!distortion_) {
                if (mode == 0) v = std::tanh(v);                                   // tape: symmetric
                else if (mode == 1) v = v >= 0 ? std::tanh(v) : std::tanh(0.7f * v) / 0.7f * 0.9f; // tube: even harmonics
                else v = v / (1.0f + std::fabs(v));                                 // soft
            } else {
                if (mode == 0) v = std::clamp(v * 3.0f, -1.0f, 1.0f);               // fuzz (hard)
                else if (mode == 1) v = std::tanh(v) * 0.9f + 0.1f * std::tanh(v * v * (v > 0 ? 1.0f : -1.0f));
                else v = std::fabs(std::tanh(v)) * 2.0f - 1.0f;                     // rectifier
            }
            hi_[static_cast<size_t>(i)] = v;
        }
        // wet = oversampled path; dry is delayed by the same latency so mix < 1 stays phase-aligned
        const int n = io.numFrames;
        float* wet = wet_.data();
        os_[ch].down(hi_.data(), wet, n);
        const int lat = os_[ch].latencySamples();
        auto& line = dryLine_[ch];
        for (int i = 0; i < n; ++i) {
            const float w = dc_[ch].process(tone_[ch].process(wet[i])) * out;
            const float dry = line[static_cast<size_t>(dryPos_[ch])];
            line[static_cast<size_t>(dryPos_[ch])] = x[i];
            dryPos_[ch] = (dryPos_[ch] + 1) % std::max(1, lat);
            x[i] = dry * (1.0f - mix) + w * mix;
        }
    }
}

} // namespace roy
