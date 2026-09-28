#pragma once
// Beat Lab step sequencer: pattern editing helpers and expansion to notes.
#include "audio/ProjectRuntime.h"
#include "project/Project.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace roy::beat {

// Resizes every row to `numSteps` (16/32/64). Existing steps are kept;
// growing repeats the existing content so the groove is preserved.
void setPatternLength(Pattern& p, int numSteps);
// Deep copy with fresh ids.
Pattern duplicatePattern(const Pattern& p, const std::string& newName);
// Variation: deterministic (seeded) small changes - ghost hats, velocity
// jitter, occasional extra kick/snare ghost notes. Never removes the backbeat.
Pattern makeVariation(const Pattern& p, uint64_t seed, float amount = 0.3f);
// Toggles a step. Returns the new on-state.
bool toggleStep(Pattern& p, int row, int step);
// Timeline position (in beats, relative to pattern start) of a step including swing
// and the pattern's groove template.
double stepPosition(const Pattern& p, int step);

// ---- groove templates ------------------------------------------------------------
// A groove is a 16-step cycle of timing offsets (fraction of a step, + = late) and
// velocity multipliers. It is applied on playback (non-destructive), scaled by
// Pattern::grooveAmount; the steps themselves are never changed.
struct GrooveTemplate {
    std::string name;
    std::string description;
    float timing[16];
    float velocity[16];
};
const std::vector<GrooveTemplate>& grooveTemplates();
const GrooveTemplate* findGroove(const std::string& name);
double grooveOffsetSteps(const Pattern& p, int step);
float grooveVelocity(const Pattern& p, int step);

// ---- velocity curves (destructive, one undo step via SetVelocityCurve) -----------
// Curves: "Flat", "Ramp Up", "Ramp Down", "Accent Downbeats", "Accent Offbeats", "Humanize".
// Only steps that are on in [from, to] change; velocities land in [lo, hi].
const std::vector<std::string>& velocityCurves();
bool applyVelocityCurve(PatternRow& row, const std::string& curve, float lo, float hi, int from, int to, uint64_t seed = 1);

// ---- note repeat ---------------------------------------------------------------
// Fills steps [from, to] of a row with repeats at `rate` ("1/4", "1/8", "1/16", "1/32",
// "1/64", "1/8T", "1/16T", "1/32T"). Rates finer than a step become ratchets (Step::roll),
// triplets use rolls spread over several steps (Step::rollLength). Assumes 1/16 steps.
const std::vector<std::string>& noteRepeatRates();
bool noteRepeat(Pattern& p, PatternRow& row, int from, int to, const std::string& rate, float velocity, std::string* error = nullptr);

// ---- generator -----------------------------------------------------------------
// Styles: "Trap", "Boom Bap", "Drill", "House", "Afro". Deterministic for a seed.
const std::vector<std::string>& generatorStyles();
bool generatePattern(Pattern& p, const std::string& style, uint64_t seed, std::string* error = nullptr);

} // namespace roy::beat
