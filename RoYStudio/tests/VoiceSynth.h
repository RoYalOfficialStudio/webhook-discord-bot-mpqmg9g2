#pragma once
// TEST SIGNAL GENERATOR (mock vocal): additive "voice" with a fixed formant
// envelope and a programmable pitch curve. Used to test pitch detection,
// Pitch Guardian, PSOLA and vocal analysis with known ground truth.
#include "core/Math.h"

#include <cmath>
#include <functional>
#include <vector>

namespace roytest {

struct VoiceSpec {
    double sr = 48000.0;
    double seconds = 1.0;
    std::function<double(double t)> midi = [](double) { return 60.0; }; // 0 = silence
    std::vector<double> formants = {700.0, 1200.0, 2600.0};
    std::vector<double> bandwidths = {130.0, 170.0, 250.0};
    float amp = 0.3f;
    double noise = 0.0; // added white noise amplitude
};

inline double formantEnvelope(const VoiceSpec& v, double f) {
    double e = 0.02;
    for (size_t i = 0; i < v.formants.size(); ++i) {
        const double d = (f - v.formants[i]) / v.bandwidths[i];
        e += std::exp(-d * d) * (i == 0 ? 1.0 : 0.6 / static_cast<double>(i));
    }
    return e;
}

inline std::vector<float> makeVoice(const VoiceSpec& v) {
    const size_t n = static_cast<size_t>(v.seconds * v.sr);
    std::vector<float> out(n, 0.0f);
    double phase = 0.0;
    roy::Rng rng(7);
    double norm = 0;
    for (double f = 80; f < 1000; f += 1) norm = std::max(norm, formantEnvelope(v, f));
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / v.sr;
        const double m = v.midi(t);
        float s = 0.0f;
        if (m > 0) {
            const double f0 = roy::midiToHz(m);
            phase += f0 / v.sr;
            if (phase > 1e6) phase -= 1e6;
            double acc = 0;
            for (int h = 1; f0 * h < v.sr * 0.45 && h < 60; ++h)
                // glottal source tilt (-6 dB/oct) shaped by the vocal-tract formant envelope
                acc += (0.25 + formantEnvelope(v, f0 * h) / norm) * std::sin(roy::kTwoPi * phase * h) / static_cast<double>(h);
            s = static_cast<float>(acc) * v.amp;
        }
        if (v.noise > 0) s += static_cast<float>(rng.uniform(-v.noise, v.noise));
        out[i] = s;
    }
    return out;
}

} // namespace roytest
