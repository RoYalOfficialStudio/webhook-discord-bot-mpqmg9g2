#pragma once
// Monophonic pitch tracking (YIN, FFT-accelerated difference function).
#include <cstdint>
#include <vector>

namespace roy::vocal {

struct PitchFrame {
    double time = 0;       // seconds (frame centre)
    double hz = 0;         // 0 = unvoiced
    double midi = 0;       // fractional MIDI pitch (0 if unvoiced)
    double confidence = 0; // 0..1 (1 - YIN aperiodicity), 0 if unvoiced
    double rmsDb = -120;   // frame level
    bool voiced = false;
};

struct PitchTrack {
    double sampleRate = 48000;
    int hop = 256;
    std::vector<PitchFrame> frames;
    double hopSeconds() const { return hop / sampleRate; }
    // Frame index for a time in seconds (clamped).
    size_t indexAt(double seconds) const;
    // Linear interpolated MIDI pitch at a time; 0 if unvoiced there.
    double midiAt(double seconds) const;
};

struct PitchDetectorSettings {
    double minHz = 60.0;
    double maxHz = 1100.0;
    double threshold = 0.15;   // YIN absolute threshold
    double minConfidence = 0.5;
    double silenceDb = -50.0;
    double windowSeconds = 0.025;
    int hop = 256;
};

PitchTrack detectPitch(const float* mono, int64_t numSamples, double sampleRate, const PitchDetectorSettings& s = {});
PitchTrack detectPitch(const std::vector<std::vector<float>>& channels, double sampleRate, const PitchDetectorSettings& s = {});

} // namespace roy::vocal
