#pragma once
// Beat Lab step sequencer: pattern editing helpers and expansion to notes.
#include "audio/ProjectRuntime.h"
#include "project/Project.h"

#include <cstdint>
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
// Timeline position (in beats, relative to pattern start) of a step including swing.
double stepPosition(const Pattern& p, int step);

} // namespace roy::beat
