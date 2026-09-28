#pragma once
// Offline analysis helpers: onset detection, framewise levels, band energy,
// LPC formants, DTW alignment, BPM and key estimation.
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace roy::dsp {

std::vector<float> mixToMono(const std::vector<std::vector<float>>& ch);

// ---- onsets ----------------------------------------------------------------
struct Onset {
    double time = 0;     // seconds
    double strength = 0; // normalized 0..1
};
struct OnsetSettings {
    int fftSize = 1024;
    int hop = 256;
    double sensitivity = 1.0; // higher = more onsets
    double minGapSeconds = 0.05;
    bool refine = true;       // refine to the energy rise in the time domain
};
// Onset detection function (log-magnitude spectral flux), one value per hop.
std::vector<float> onsetFunction(const float* x, int64_t n, double sr, int fftSize, int hop);
std::vector<Onset> detectOnsets(const float* x, int64_t n, double sr, const OnsetSettings& s = {});

// ---- framewise measurements ------------------------------------------------------
struct FrameLevels {
    double hopSeconds = 0.01;
    std::vector<float> rmsDb;
    std::vector<float> peakDb;
};
FrameLevels frameLevels(const float* x, int64_t n, double sr, double frameSeconds = 0.05, double hopSeconds = 0.025);
// Energy (linear power) in [lo, hi) Hz per frame.
std::vector<float> bandEnergy(const float* x, int64_t n, double sr, double lo, double hi, int fftSize, int hop);
// Long-term average magnitude spectrum (dB), `bins` = fftSize/2+1.
std::vector<float> averageSpectrumDb(const float* x, int64_t n, double sr, int fftSize = 4096);
double percentile(std::vector<float> v, double p);

// ---- LPC formants ------------------------------------------------------------------
// Returns up to `count` formant frequencies (Hz) of a voiced segment.
std::vector<double> estimateFormants(const float* x, int64_t n, double sr, int count = 3);

// ---- DTW ----------------------------------------------------------------------------
// Aligns feature sequences a (reference) and b (to align). Returns the warping
// path as (indexA, indexB) pairs from start to end. `band` limits |i - j|.
std::vector<std::pair<int, int>> dtwAlign(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b,
                                          int band);
// Frame features for alignment: [log energy, onset strength, spectral centroid] (z-scored).
std::vector<std::vector<float>> alignmentFeatures(const float* x, int64_t n, double sr, int hop);

// ---- tempo & key --------------------------------------------------------------------
struct TempoEstimate {
    double bpm = 0;
    double confidence = 0;
};
TempoEstimate estimateBpm(const float* x, int64_t n, double sr, double minBpm = 60, double maxBpm = 180);
struct KeyEstimate {
    int root = 0;
    bool minor = false;
    double confidence = 0; // correlation difference to the runner-up
    std::vector<float> chroma;
};
KeyEstimate estimateKey(const float* x, int64_t n, double sr);

} // namespace roy::dsp
