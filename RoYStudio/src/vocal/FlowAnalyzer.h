#pragma once
// FLOW ANALYZER (rap): technical timing data - onsets, grid positions,
// EARLY / ON BEAT / LATE, kick/snare relation, pauses, phrases, density.
// Onsets approximate syllable starts; this is measurement, not a rating.
#include "audio/TempoMap.h"
#include "project/Project.h"

#include <string>
#include <vector>

namespace roy::vocal {

enum class Placement { Early, OnBeat, Late };
const char* placementName(Placement p);

struct FlowOnset {
    double timeSec = 0;       // timeline seconds
    double beat = 0;          // timeline beat
    double gridBeat = 0;      // nearest grid position
    double offsetMs = 0;      // + = late
    Placement placement = Placement::OnBeat;
    double kickOffsetMs = 1e9;  // to the nearest kick (if known)
    double snareOffsetMs = 1e9; // to the nearest snare (if known)
    int phrase = -1;
    double strength = 0;
};

struct FlowPhrase {
    double startSec = 0, endSec = 0;
    double startBeat = 0, lengthBeats = 0;
    int onsets = 0;
    double onsetsPerBeat = 0;
    double onsetsPerSecond = 0;
    double meanOffsetMs = 0;
};

struct FlowSettings {
    double gridBeats = 0.25;          // 1/16 notes
    double onBeatToleranceMs = 20.0;
    double pauseSeconds = 0.25;       // gap that separates phrases
    double sensitivity = 1.0;
};

struct FlowReport {
    std::vector<FlowOnset> onsets;
    std::vector<FlowPhrase> phrases;
    std::vector<std::pair<double, double>> pauses; // seconds
    double meanOffsetMs = 0, offsetStdMs = 0;
    double earlyRatio = 0, onRatio = 0, lateRatio = 0;
    double onsetsPerSecond = 0;
    std::string note = "Onsets approximate syllables; values are technical measurements, not a quality rating.";
};

// `vocal` starts at timeline second `startSec`. kicks/snares are timeline beats.
FlowReport analyzeFlow(const std::vector<std::vector<float>>& vocal, double sampleRate, double startSec, const TempoMap& tempo,
                       const std::vector<double>& kickBeats, const std::vector<double>& snareBeats,
                       const FlowSettings& s = {});

// Kick / snare positions (timeline beats) from Beat Lab pattern clips and MIDI drum notes (36/38).
std::vector<double> drumHitsFromProject(const Project& p, const std::string& voice, double fromBeat, double toBeat);

} // namespace roy::vocal
