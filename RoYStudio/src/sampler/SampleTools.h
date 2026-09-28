#pragma once
// Offline sample editing and analysis for the Sampler / Browser.
// All functions return new buffers; inputs are never modified.
#include "dsp/Analysis.h"
#include "instruments/Sampler.h"

#include <string>
#include <vector>

namespace roy::sampler {

using Channels = std::vector<std::vector<float>>;

Channels trim(const Channels& in, int64_t start, int64_t end);
// Removes leading/trailing silence below thresholdDb.
Channels trimSilence(const Channels& in, double thresholdDb = -60.0, int64_t* removedStart = nullptr);
Channels normalize(const Channels& in, double targetPeakDb = -0.3);
Channels reverse(const Channels& in);
Channels fade(const Channels& in, int64_t fadeInSamples, int64_t fadeOutSamples);

// Transient positions (samples).
std::vector<int64_t> detectTransients(const Channels& in, double sampleRate, double sensitivity = 1.0);
// Slice boundaries: transients or an equal grid of `gridSlices`.
std::vector<std::pair<int64_t, int64_t>> slicesFromTransients(const Channels& in, double sampleRate, double sensitivity = 1.0);
std::vector<std::pair<int64_t, int64_t>> slicesGrid(int64_t length, int count);
// SLICE TO PADS: one one-shot zone per slice, mapped to consecutive notes from `firstNote`.
std::vector<SamplerZone> sliceToPads(const std::string& assetId, const std::vector<std::pair<int64_t, int64_t>>& slices,
                                     int firstNote = 36, bool chokeTogether = false);

// Root note of a pitched sample (MIDI, rounded) and its fine tuning in cents.
struct RootNote {
    int note = -1;
    double cents = 0;
    double confidence = 0;
};
RootNote detectRootNote(const Channels& in, double sampleRate);

struct SampleInfo {
    double durationSec = 0;
    double peakDb = -120;
    double bpm = 0, bpmConfidence = 0;
    int keyRoot = -1;
    bool keyMinor = false;
    double keyConfidence = 0;
    RootNote root;
    int transients = 0;
    bool looksLikeLoop = false; // long enough and rhythmic
};
SampleInfo analyzeSample(const Channels& in, double sampleRate);

} // namespace roy::sampler
