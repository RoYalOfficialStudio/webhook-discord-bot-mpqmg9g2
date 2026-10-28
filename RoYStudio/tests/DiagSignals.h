#pragma once
// GMB 13 test signals shared by the diagnosis and VocalTune 2 tests (smooth reference voice, tones,
// glides, vibrato, fades) and a block-pattern processor runner.
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "audio/Processor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

namespace roytest::gmb13 {
using namespace roy;
constexpr double SR = 48000.0;

inline std::vector<float> tone(double hz, double seconds, float amp, double sr = SR) {
    std::vector<float> v(static_cast<size_t>(sr * seconds));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / sr));
    return v;
}
inline std::vector<float> glide(double hz0, double hz1, double seconds, float amp) {
    std::vector<float> v(static_cast<size_t>(SR * seconds));
    double ph = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(v.size());
        ph += (hz0 * std::pow(hz1 / hz0, t)) / SR;
        v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * ph));
    }
    return v;
}
inline std::vector<float> vibrato(double hz, double rateHz, double cents, double seconds, float amp) {
    std::vector<float> v(static_cast<size_t>(SR * seconds));
    double ph = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        ph += hz * std::pow(2.0, cents / 1200.0 * std::sin(2 * std::numbers::pi * rateHz * t)) / SR;
        v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * ph));
    }
    return v;
}
// Smooth reference voice for the diagnosis tests: harmonics with a formant envelope (+ a flat source part
// so the fundamental stays present, like a real chest voice), -12 dB/oct source
// tilt and a soft high-frequency taper (6..9 kHz) so harmonics fade in/out continuously while the pitch
// moves. (tests/VoiceSynth.h switches harmonics on/off abruptly at 0.45*sr and has a sawtooth-like edge -
// fine for pitch tests, but it really contains small discontinuities, which a click detector must report.)
inline std::vector<float> smoothVoice(const std::function<double(double)>& midiAt, double seconds, float amp = 0.3f) {
    VoiceSpec v; // formants only
    const size_t n = static_cast<size_t>(seconds * SR);
    std::vector<float> out(n);
    double phase = 0;
    for (size_t i = 0; i < n; ++i) {
        const double f0 = midiToHz(midiAt(static_cast<double>(i) / SR));
        phase += f0 / SR;
        phase -= std::floor(phase);
        double acc = 0;
        for (int h = 1; h < 80; ++h) {
            const double fh = f0 * h;
            if (fh >= 9000) break;
            const double taper = fh < 6000 ? 1.0 : 0.5 + 0.5 * std::cos(std::numbers::pi * (fh - 6000) / 3000);
            acc += taper * (0.3 + formantEnvelope(v, fh)) * std::sin(2 * std::numbers::pi * phase * h) / (static_cast<double>(h) * h);
        }
        out[i] = static_cast<float>(acc);
    }
    const float pk = peak(out);
    for (auto& x : out) x *= amp / std::max(pk, 1e-9f);
    return out;
}

// continuous sung melody (no silences), notes slightly off C major, 0.4 s each, 40 ms glides
inline std::vector<float> melody(double seconds = 3.2) {
    return smoothVoice([](double t) {
        static const double notes[] = {60.3, 62.4, 64.35, 65.6, 67.3, 69.45, 71.3, 72.4};
        const int k = static_cast<int>(t / 0.4), i = k % 8;
        const double local = t - k * 0.4; // (consistent with k - fmod could disagree at exact multiples)
        const double prev = notes[(i + 7) % 8];
        if (k > 0 && local < 0.04) return prev + (notes[i] - prev) * (local / 0.04);
        return notes[i];
    }, seconds);
}

// 10 ms raised-cosine fade-in (a signal that starts with full slope at sample 0 IS a discontinuity)
inline std::vector<float> faded(std::vector<float> x) {
    const size_t n = std::min<size_t>(x.size(), 480);
    for (size_t i = 0; i < n; ++i) x[i] *= static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(i) / n));
    return x;
}
inline std::vector<float> withSilence(std::vector<float> x, double before) {
    x.insert(x.begin(), static_cast<size_t>(before * SR), 0.0f);
    return x;
}

// processes a mono signal through a processor (dual mono, stereo block) with a block-size pattern
inline std::vector<float> runMono(Processor& p, const std::vector<float>& x, const std::vector<int>& blocks) {
    std::vector<float> l = x, r = x;
    size_t off = 0, bi = 0;
    while (off < x.size()) {
        const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(blocks[bi++ % blocks.size()]), x.size() - off));
        float* ptr[2] = {l.data() + off, r.data() + off};
        AudioBlock b{ptr, 2, k};
        p.process(b, nullptr, nullptr, 0);
        off += static_cast<size_t>(k);
    }
    return l;
}

inline const std::vector<std::vector<int>>& blockPatterns() {
    static const std::vector<std::vector<int>> b = {{64}, {256}, {512}, {562}, {1024}, {1, 7, 100, 333, 64, 1024, 3, 562}};
    return b;
}
inline std::string patternName(const std::vector<int>& b) { return b.size() == 1 ? std::to_string(b[0]) : std::string("irregular"); }
} // namespace roytest::gmb13
