#pragma once
// MOCK / SYNTHETIC vocal test material with ground truth (clearly labelled test data, generated here).
// More realistic than VoiceSynth: glottal source with jitter + shimmer, breathiness,
// formant vowels, unvoiced consonants (sibilants, plosives), breaths and background noise.
#include "TestHelpers.h"

#include "core/Math.h"

#include <cmath>
#include <vector>

namespace roytest {

enum class SegKind { Note, Slide, Rest, Breath, Sibilant, Plosive };

struct Seg {
    SegKind kind = SegKind::Note;
    double seconds = 0.4;
    double midi = 60;        // Note: pitch (fractional = detuned); Slide: start pitch
    double midiTo = 60;      // Slide: end pitch
    double vibratoCents = 0; // Note: vibrato depth (+-)
    double vibratoHz = 5.5;
    double amp = 0.3;
};

struct TruthNote {
    double start, end, midi;
    SegKind kind;
};

struct VocalSpec {
    double sr = 48000.0;
    std::vector<Seg> segs;
    std::vector<double> formants = {730, 1090, 2440}; // "ah"
    double jitterCents = 8;   // random pitch micro-variation (natural voice)
    double shimmer = 0.08;    // random amplitude variation
    double breathiness = 0.02;
    double noiseDb = -90;     // background noise level (dBFS RMS)
    uint64_t seed = 11;
};

struct VocalTake {
    std::vector<float> audio;
    std::vector<TruthNote> truth;
    double sr = 48000.0;
};

inline VocalTake makeVocal(const VocalSpec& v) {
    VocalTake out;
    out.sr = v.sr;
    roy::Rng rng(v.seed);
    double t0 = 0, phase = 0, jitter = 0, shimmer = 0;
    double lp = 0, hp = 0, prevNoise = 0; // simple filters for noise colours
    const double noiseAmp = std::pow(10.0, v.noiseDb / 20.0) * std::sqrt(3.0);
    for (const Seg& s : v.segs) {
        const size_t n = static_cast<size_t>(s.seconds * v.sr);
        if (s.kind == SegKind::Note || s.kind == SegKind::Slide) out.truth.push_back({t0, t0 + s.seconds, s.midi, s.kind});
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / v.sr;
            const double x = t / s.seconds;
            double y = 0;
            // slowly varying jitter/shimmer (random walk, bounded)
            jitter = 0.999 * jitter + 0.001 * rng.uniform(-1, 1) * 30.0;
            shimmer = 0.998 * shimmer + 0.002 * rng.uniform(-1, 1) * 10.0;
            switch (s.kind) {
            case SegKind::Note:
            case SegKind::Slide: {
                double m = s.kind == SegKind::Note ? s.midi : s.midi + (s.midiTo - s.midi) * (0.5 - 0.5 * std::cos(roy::kPi * x));
                m += s.vibratoCents / 100.0 * std::sin(roy::kTwoPi * s.vibratoHz * t) * std::min(1.0, t / 0.15);
                m += std::clamp(jitter, -1.0, 1.0) * v.jitterCents / 100.0;
                const double f0 = roy::midiToHz(m);
                phase += f0 / v.sr;
                double acc = 0;
                for (int h = 1; f0 * h < v.sr * 0.45 && h < 50; ++h) {
                    const double f = f0 * h;
                    double env = 0.03;
                    for (size_t k = 0; k < v.formants.size(); ++k) {
                        const double d = (f - v.formants[k]) / (90.0 + 40.0 * k);
                        env += std::exp(-d * d) * (k == 0 ? 1.0 : 0.5);
                    }
                    acc += (0.3 + env) * std::sin(roy::kTwoPi * phase * h) / h;
                }
                const double attack = std::min(1.0, t / 0.02), release = std::min(1.0, (s.seconds - t) / 0.03);
                const double sh = 1.0 + std::clamp(shimmer, -1.0, 1.0) * v.shimmer;
                y = s.amp * attack * release * sh * (acc * 0.5 + v.breathiness * rng.uniform(-1, 1));
                break;
            }
            case SegKind::Breath: { // band-limited aspiration noise
                const double w = rng.uniform(-1, 1);
                lp += 0.15 * (w - lp);
                y = s.amp * 0.3 * std::sin(roy::kPi * x) * lp * 4.0;
                break;
            }
            case SegKind::Sibilant: { // high-passed noise ("s")
                const double w = rng.uniform(-1, 1);
                hp = w - prevNoise + 0.2 * hp;
                prevNoise = w;
                y = s.amp * std::sin(roy::kPi * x) * hp * 0.5;
                break;
            }
            case SegKind::Plosive: { // short low-frequency burst ("p")
                y = s.amp * 1.5 * std::exp(-t / 0.008) * std::sin(roy::kTwoPi * 70.0 * t) + s.amp * 0.3 * std::exp(-t / 0.004) * rng.uniform(-1, 1);
                break;
            }
            case SegKind::Rest: break;
            }
            if (noiseAmp > 1e-7) y += noiseAmp * rng.uniform(-1, 1);
            out.audio.push_back(static_cast<float>(y));
        }
        t0 += s.seconds;
    }
    return out;
}

// Rap-like phrase: short syllables, speech intonation (glides of several semitones), consonants in between.
inline VocalSpec rapSpec(double baseMidi = 50, uint64_t seed = 3) {
    VocalSpec v;
    v.seed = seed;
    roy::Rng r(seed);
    for (int i = 0; i < 24; ++i) {
        if (i % 3 == 0) v.segs.push_back({SegKind::Sibilant, 0.05, 0, 0, 0, 0, 0.15});
        const double start = baseMidi + r.uniform(-2.0, 3.0);
        v.segs.push_back({SegKind::Slide, r.uniform(0.09, 0.16), start, start + r.uniform(-3.0, 1.0), 0, 0, 0.3});
        if (i % 4 == 3) v.segs.push_back({SegKind::Plosive, 0.04, 0, 0, 0, 0, 0.3});
        v.segs.push_back({SegKind::Rest, r.uniform(0.02, 0.06)});
    }
    return v;
}

} // namespace roytest
