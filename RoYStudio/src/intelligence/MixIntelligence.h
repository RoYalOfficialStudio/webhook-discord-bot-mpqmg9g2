#pragma once
// MIX INTELLIGENCE: technical mix analysis. Produces measurements, problems
// and suggested (undoable) commands. It never changes the mix by itself.
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace roy {
class AudioEngine;
class ProjectRuntime;
}

namespace roy::mixi {

using Channels = std::vector<std::vector<float>>;

struct TrackAudio {
    std::string trackId, channelId, name, role; // role: "vocal", "808", "drums", ... (free text)
    Channels audio;                             // post-fader stereo output
};

struct MixSuggestion {
    std::string description;
    std::string command; // command id, empty = manual action
    nlohmann::json args;
};

struct MixIssue {
    std::string type;    // masking | clipping | low_end_collision | resonance | sibilance | stereo | phase | headroom
    std::string severity; // info | warning | problem
    std::vector<std::string> tracks;
    double lowHz = 0, highHz = 0;
    std::string title;   // "VOCAL <-> PIANO MASKING 2.2-3.1 kHz"
    std::string detail;
    double value = 0;    // main measured value (meaning depends on type)
    std::vector<MixSuggestion> suggestions;
};

struct MixReport {
    std::vector<MixIssue> issues;
    double masterPeakDb = -120, masterTruePeakDb = -120, masterLufs = -70, masterCorrelation = 1, masterLowSideRatioDb = -120;
    nlohmann::json toJson() const;
};

struct MixSettings {
    double maskingMinFraction = 0.3; // share of active frames in which a band is masked
    double maskingMarginDb = 3.0;
    double sampleRate = 48000;
};

MixReport analyzeMix(const std::vector<TrackAudio>& tracks, const Channels& master, const MixSettings& s = {});

// Renders all track channels + master in one offline pass (device must be stopped).
bool renderForAnalysis(AudioEngine& engine, ProjectRuntime& runtime, const Project& project, double startBeat, double endBeat,
                       std::vector<TrackAudio>& tracks, Channels& master, std::string* error = nullptr);

// Third-octave band centres used by the masking analysis (25 Hz .. 16 kHz).
const std::vector<double>& thirdOctaveCentres();

} // namespace roy::mixi
