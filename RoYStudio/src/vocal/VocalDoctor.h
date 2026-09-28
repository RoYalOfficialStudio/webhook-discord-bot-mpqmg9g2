#pragma once
// VOCAL DOCTOR - technical analysis of a vocal recording.
// Reports measured values and concrete technical issues with suggested,
// undoable fixes (expressed as commands). Not a judgement of the performance.
#include "midi/Scale.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace roy::vocal {

using json = nlohmann::json;

struct DoctorMeasurements {
    double durationSec = 0;
    double peakDb = -120, rmsDb = -120, crestDb = 0;
    uint32_t clippedSamples = 0, clipEvents = 0;
    double dcOffset = 0;
    double noiseFloorDb = -120;   // 10th percentile of 50 ms frame RMS
    double activeLevelDb = -120;  // median RMS of active frames
    double snrDb = 0;
    double silenceRatio = 0;      // frames below -60 dBFS
    double rumbleRatioDb = -120;  // energy < 80 Hz relative to total
    double sibilanceRatioDb = -120; // 5-10 kHz vs total, loudest 5 % of active frames
    int sibilantEvents = 0;
    int plosiveEvents = 0;
    std::vector<std::pair<double, double>> breaths; // [start, end] seconds
    std::vector<double> plosiveTimes;
    std::vector<double> resonancesHz;
    double harshnessDb = 0;       // 2-5 kHz vs 200 Hz-2 kHz band level
    double dynamicRangeDb = 0;    // 95th - 10th percentile of active frame RMS
    double pitchMedianMidi = 0, pitchLowMidi = 0, pitchHighMidi = 0;
    double inTuneRatio = 0;       // stable notes within +-25 cents
    int offKeyNotes = 0;
};

struct SuggestedFix {
    std::string id;
    std::string description;
    std::string command; // command registry id
    json args;           // command arguments (channelId/clipId filled in by the caller)
};

struct DoctorIssue {
    std::string id;       // "clipping", "dc_offset", "rumble", "noise", "sibilance", "plosives", "breaths",
                          // "resonance", "harshness", "dynamics", "level", "pitch"
    std::string severity; // "info" | "warning" | "problem"
    std::string title;
    std::string detail;   // measured value vs threshold
    std::vector<SuggestedFix> fixes;
};

struct DoctorReport {
    DoctorMeasurements m;
    std::vector<DoctorIssue> issues;
    json toJson() const;
};

struct DoctorSettings {
    bool analysePitch = true;
    Key key;
    bool keyKnown = false;
};

DoctorReport examineVocal(const std::vector<std::vector<float>>& audio, double sampleRate, const DoctorSettings& s = {});

} // namespace roy::vocal
