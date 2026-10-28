#pragma once
// GMB 13.0 - CLICK / DISCONTINUITY DETECTOR (offline analysis, not for the audio thread).
//
// Finds sample-level discontinuities: a second-order linear predictor x^[n] = 2x[n-1] - x[n-2]
// follows smooth audio (sines, glides, vibrato, voices) closely; a click, a dropout edge, a jump
// between two blocks or a hard parameter step leaves a large prediction error e[n] compared to the
// local level of e around it. score = |e[n]| / (RMS of e in +-window, centre excluded) - for smooth
// material the score stays low, for a discontinuity it is high.
// It measures "is there a discontinuity" - it cannot say whether it is audible (needs a listening test)
// and it does not know WHY it happened (DSP artefact vs. device dropout: compare with CallbackTiming).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace roy::diag {

struct ClickOptions {
    double threshold = 12.0;      // score above which a sample counts as a click
    double absFloor = 2e-3;       // |e| must exceed this (ignores noise floor / near silence)
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
