#pragma once
// KICK / 808 COLLISION ANALYZER. Measures, suggests - never changes anything.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace roy::beat {

struct CollisionSuggestion {
    std::string type; // "sidechain" | "dynamic_eq" | "envelope" | "phase"
    std::string description;
    nlohmann::json params;
};

struct CollisionReport {
    double kickFundamentalHz = 0, bassFundamentalHz = 0;
    double overlapRatio = 0;     // share of the kick's low-band energy that coincides with 808 energy
    double correlation = 0;      // low-band (<150 Hz) correlation during the overlap
    double sumVsSeparateDb = 0;  // combined low energy vs sum of separate energies (<0 = cancellation)
    double kickPeakDb = -120, bassPeakDb = -120, combinedPeakDb = -120;
    double kickLowDb = -120, bassLowDb = -120, combinedLowDb = -120;
    double kickDecayMs = 0;      // time until the kick's low band falls 20 dB below its peak
    double bestBassDelayMs = 0;  // bass delay (0..15 ms) that maximises combined low energy
    bool invertBassHelps = false;
    std::vector<CollisionSuggestion> suggestions;
    nlohmann::json toJson() const;
};

// `bassOffsetSec` = start of the 808 relative to the kick (both signals mono).
CollisionReport analyzeKick808(const std::vector<float>& kick, const std::vector<float>& bass, double sampleRate,
                               double bassOffsetSec = 0.0);

// Curves for the visual KICK <-> 808 analyzer (UI). Time axis in 5 ms hops from the kick
// start, spectrum on a log axis 20..400 Hz. All values are measurements, nothing is changed.
struct Kick808Visual {
    std::vector<float> timeMs, kickEnvDb, bassEnvDb; // low band (<150 Hz) envelopes, dB re. the louder peak
    std::vector<float> correlation;                  // running low-band correlation (20 ms window), NaN when silent
    std::vector<float> freqHz, kickSpecDb, bassSpecDb; // magnitude spectra, dB re. the louder peak
    double frequencyOverlap = 0; // shared spectral energy / kick spectral energy (0..1)
    double timingOverlapMs = 0;  // time both envelopes are within 20 dB of their own peak
    double phaseCorrelation = 0; // energy-weighted correlation while both sound (-1..1)
    nlohmann::json toJson() const;
};
Kick808Visual analyzeKick808Visual(const std::vector<float>& kick, const std::vector<float>& bass, double sampleRate,
                                   double bassOffsetSec = 0.0, double lengthSec = 0.6);

// ROOT DETECTION for 808 / bass one-shots: measures the pitch in three windows after
// the attack (pitch envelopes settle), reports the settled note and how stable it is.
struct RootDetection {
    double hz = 0;
    int midiNote = -1;
    double cents = 0;       // deviation of hz from midiNote
    double confidence = 0;  // 0..1 (window agreement + tonal energy share)
    std::string noteName;   // "C#1"
    nlohmann::json toJson() const;
};
RootDetection detectRoot(const float* x, int64_t n, double sampleRate);

// Fundamental (Hz) of a low-frequency one-shot (kick/808), searching 25..250 Hz.
double lowFundamental(const float* x, int64_t n, double sampleRate, double skipSeconds = 0.01);

} // namespace roy::beat
