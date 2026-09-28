#pragma once
// VOCAL DNA (own-voice profile, local only) and PROJECT ASSISTANT.
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace roy {
class ProjectRuntime;
}

namespace roy::assist {

// ---- VOCAL DNA ------------------------------------------------------------------
// Technical characteristics learned ONLY from the user's own recordings, stored
// locally (project or an explicit file). Used for analysis/editing of the user's
// own performances - never to imitate anyone's voice.
struct VocalDna {
    int takes = 0;
    double seconds = 0;
    double pitchLowMidi = 0, pitchMedianMidi = 0, pitchHighMidi = 0;
    double dynamicRangeDb = 0;
    double vibratoRateHz = 0, vibratoExtentCents = 0;
    double meanPhraseSeconds = 0;
    double inTuneRatio = 0;
    double sustainedRatio = 0;
    nlohmann::json toJson() const;
    static VocalDna fromJson(const nlohmann::json& j);
};
// Adds one recording to the profile (running averages weighted by duration).
void learnVocal(VocalDna& dna, const std::vector<std::vector<float>>& audio, double sampleRate);
struct DnaDeviation {
    std::string key, text;
    double value = 0, usual = 0;
};
std::vector<DnaDeviation> compareToDna(const VocalDna& dna, const std::vector<std::vector<float>>& take, double sampleRate);

// ---- PROJECT ASSISTANT ------------------------------------------------------------
struct Finding {
    std::string id;       // missing_file | unused_asset | empty_track | solo_active | ...
    std::string severity; // info | warning | problem
    std::string text;
    std::string command;  // optional fix
    nlohmann::json args;
};
std::vector<Finding> checkProject(const Project& p, const std::filesystem::path& projectFolder, bool dirty = false,
                                  size_t backupCount = 0);

} // namespace roy::assist
