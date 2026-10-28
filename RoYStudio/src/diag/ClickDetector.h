#pragma once
// GMB 13.0 - CLICK / DISCONTINUITY DETECTOR (offline analysis, not for the audio thread).
//
// Finds sample-level discontinuities that a smooth or periodic signal cannot contain:
//   1. short-term linear prediction (LPC order 24, per 512-sample hop, 2048 Hann window) removes the
//      spectral envelope (formants, tones) -> residual e1;
//   2. long-term (pitch) prediction removes the periodic glottal pulses of a voice:
//      e2[n] = e1[n] - b * (e1[n-T] + e1[n+T]) / 2, T = best period in 40..800 samples;
//   3. score[n] = |e2[n]| / RMS of e2 in +-window (centre excluded).
// A click, a dropout edge, a skipped block or a hard gain/polarity step keeps a high score; tones,
// glides, vibrato and sung vowels do not. It measures discontinuities - audibility needs a listening
// test - and it does not know the cause (DSP artefact vs. device dropout: compare with CallbackTiming).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace roy::diag {

struct ClickOptions {
    double threshold = 12.0;      // score above which a sample counts as a click
    double absFloor = 1e-3;       // |residual| must exceed this (ignores noise floor / near silence)
    int windowSamples = 256;      // half window of the local level
    int mergeSamples = 96;        // detections closer than this are one event (2 ms at 48 kHz)
};

struct ClickEvent {
    int64_t sample = 0;
    double score = 0;
    double error = 0; // |prediction error|
};

struct ClickReport {
    std::vector<ClickEvent> events;
    double maxScore = 0;
    int64_t maxScoreSample = -1;
    size_t count() const { return events.size(); }
};

ClickReport detectClicks(const float* x, size_t n, const ClickOptions& opt = {});
inline ClickReport detectClicks(const std::vector<float>& x, const ClickOptions& opt = {}) { return detectClicks(x.data(), x.size(), opt); }

// Largest absolute difference between two signals (block-independence check).
double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0);

} // namespace roy::diag
