#pragma once
// Vocal Microscope, Double Magnet and Ghost Take.
#include "vocal/PitchDetector.h"

#include <string>
#include <vector>

namespace roy::vocal {

using Channels = std::vector<std::vector<float>>;

// ---- VOCAL MICROSCOPE ----------------------------------------------------------
struct MicroscopeReport {
    double start = 0, end = 0;          // seconds within the buffer
    // pitch
    double pitchMedianMidi = 0, pitchMinMidi = 0, pitchMaxMidi = 0;
    double pitchStabilityCents = 0;     // std-dev of voiced pitch
    double voicedRatio = 0;
    // timing
    std::vector<double> onsets;         // seconds (absolute in buffer)
    // gain
    double peakDb = -120, rmsDb = -120;
    // formants
    std::vector<double> formantsHz;     // F1..F3 (voiced part)
    // sibilance / breath / noise / transient
    double sibilanceRatioDb = -120;     // 5-10 kHz vs total
    bool looksLikeBreath = false;
    double noiseFloorDb = -120;
    double attackMs = 0;                // 10 % -> 90 % of the first onset
};
MicroscopeReport inspectRegion(const Channels& audio, double sampleRate, double startSec, double endSec);

// Region-only processing helpers (return a modified COPY; input untouched).
Channels applyRegionGain(const Channels& audio, double sampleRate, double startSec, double endSec, double gainDb,
                         double fadeMs = 5.0);
Channels applyRegionPitchShift(const Channels& audio, double sampleRate, double startSec, double endSec, double semitones,
                               bool formantPreserve = true);

// ---- DOUBLE MAGNET ---------------------------------------------------------------
enum class MagnetMode { Loose, Natural, Tight, UltraTight };
const char* magnetModeId(MagnetMode m);
MagnetMode magnetModeFromId(const std::string& s);

struct MagnetSettings {
    MagnetMode mode = MagnetMode::Natural;
    bool alignTiming = true;
    bool alignPitch = false;
    double pitchStrength = 0.7;     // 0..1
    double maxOffsetMs = 250.0;
    double maxPitchDiffSemitones = 1.5; // larger differences (harmonies) are left alone
};

struct MagnetReport {
    double meanAbsOffsetBeforeMs = 0, meanAbsOffsetAfterMs = 0;
    double maxOffsetBeforeMs = 0;
    double within20msBefore = 0, within20msAfter = 0; // fraction of active frames
    double meanPitchDiffBeforeCents = 0, meanPitchDiffAfterCents = 0;
};

struct MagnetResult {
    Channels audio;          // aligned double (new buffer; originals untouched)
    MagnetReport report;
    std::vector<double> offsetMs; // measured offset per alignment frame (double relative to main)
    double hopSeconds = 0.005;
};
MagnetResult alignDouble(const Channels& mainVocal, const Channels& doubleVocal, double sampleRate, const MagnetSettings& s);

// Measured timing offsets of `b` relative to `a` per alignment frame (ms; + = b later).
std::vector<double> measureOffsets(const Channels& a, const Channels& b, double sampleRate, double maxOffsetMs,
                                   double hopSeconds, std::vector<bool>* activeMask = nullptr);

// ---- GHOST TAKE ------------------------------------------------------------------
struct GhostComparison {
    double hopSeconds = 0.005;
    std::vector<double> timingOffsetMs;    // current vs previous per frame (active frames, else NaN)
    std::vector<double> pitchDiffCents;    // current - previous where both voiced (else NaN)
    std::vector<float> previousPeaks;      // waveform overlay of the previous take (abs peak per frame)
    std::vector<float> currentPeaks;
    double meanAbsTimingMs = 0;
    double meanAbsPitchCents = 0;
};
GhostComparison compareTakes(const Channels& previous, const Channels& current, double sampleRate);

} // namespace roy::vocal
