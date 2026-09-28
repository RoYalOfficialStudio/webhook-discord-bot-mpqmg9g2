#pragma once
// Automation helpers on the project model (message thread).
#include "project/Project.h"

#include <optional>

namespace roy::automation {

// Value of a lane at `beat` using the point curves (same maths as the audio thread).
std::optional<double> laneValueAt(const AutomationLane& lane, double beat);
// The master's "tempo" lane (enabled, with points), if any.
const AutomationLane* tempoLane(const Project& p);
// Renders the tempo lane into the project's tempo map: Hold segments become one tempo
// event, ramps (Linear / Smooth / Bezier) become steps every `resolutionBeats`
// (default 1/32 note), each step using the curve value at its midpoint.
// Without a tempo lane the tempo map is left alone. Returns true if it changed.
bool applyTempoAutomation(Project& p, double resolutionBeats = 0.125);

} // namespace roy::automation
