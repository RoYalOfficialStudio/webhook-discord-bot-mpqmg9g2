#pragma once
// MASTERING: master chain presets, Master Assistant and reference comparison.
// Presets offer targets; the user keeps control. No blanket "maximum loudness":
// the assistant reports gain reduction and refuses to hide over-limiting.
#include "dsp/Loudness.h"
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace roy::master {

using Channels = std::vector<std::vector<float>>;

struct ChainModule {
    std::string typeId;
    std::string name;
    nlohmann::json params;
};

struct MasterPreset {
    std::string id, name;
    double targetLufs = -14.0;
    double ceilingDb = -1.0;
    std::vector<ChainModule> chain; // EQ, compression, saturation, stereo, clipper, limiter, metering
};

const std::vector<MasterPreset>& presets();
const MasterPreset* findPreset(const std::string& id);

struct AssistantResult {
    dsp::LoudnessStats before;      // pre-chain
    dsp::LoudnessStats after;       // after chain with the suggested limiter input gain
    double limiterInputGainDb = 0;  // suggestion
    double maxGainReductionDb = 0;  // limiter GR at the suggestion
    bool targetReached = false;
    std::vector<std::string> notes; // honest remarks (over-limiting, dynamic range loss, ...)
};
// Runs the preset chain offline on a pre-master mix and searches the limiter
// input gain that reaches the target loudness. `maxGainReductionDb` caps the
// search (default 6 dB) so the result never relies on extreme limiting.
AssistantResult assist(const Channels& preMaster, double sampleRate, const MasterPreset& preset, double maxGainReductionDb = 6.0);
Channels processChain(const Channels& in, double sampleRate, const MasterPreset& preset, double limiterInputGainDb,
                      double* maxGrOut = nullptr);

struct ReferenceComparison {
    dsp::LoudnessStats mix, reference;
    std::vector<double> bandDiffDb; // (mix - reference) per third-octave band, loudness matched
    double correlationMix = 1, correlationRef = 1;
    std::vector<std::string> notes;
};
ReferenceComparison compareToReference(const Channels& mix, const Channels& reference, double sampleRate);

} // namespace roy::master
