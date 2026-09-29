#pragma once
// MIDI LEARN: hardware controllers (CC knobs/faders) drive mixer and plugin parameters.
// The mapping lives in the project (Project::midiMappings, commands AddMidiMapping /
// RemoveMidiMapping, undoable). Controller moves themselves are applied on the message thread
// straight to the model and the running engine - like dragging a fader, they are not undo
// steps; the project is marked modified. Mapped controllers are not passed on to instruments.
#include "project/Project.h"

#include <cstdint>
#include <string>
#include <vector>

namespace roy {
class ProjectRuntime;
}

namespace roy::midi {

struct ControlChange {
    uint8_t channel = 0; // 0..15
    uint8_t cc = 0;      // 0..127
    uint8_t value = 0;   // 0..127
};

// CC 120..127 are channel mode messages (all notes off, reset ...): never learnable.
inline bool learnableCc(int cc) { return cc >= 0 && cc < 120; }

struct MidiTarget {
    std::string channelId; // owner channel (also for slot parameters)
    std::string slotId;    // empty = channel strip
    std::string paramId;   // "gain" | "pan" | "width" | "send:<id>" | processor param id
};

// Resolves the owner channel for a slot target and checks that the target exists. Fills the
// default range the CC spans (gain/send -60..+6 dB, pan -1..1, width 0..2, processor: its range).
bool resolveTarget(Project& p, ProjectRuntime* rt, MidiTarget& t, float* minValue = nullptr, float* maxValue = nullptr,
                   std::string* error = nullptr);
bool mappingTargetExists(Project& p, ProjectRuntime* rt, const MidiMapping& m);
// "Vocal · Volume", "Vocal · RoY EQ · Low Gain" ...
std::string targetName(Project& p, ProjectRuntime* rt, const MidiMapping& m);
const MidiMapping* findMappingForTarget(const Project& p, const std::string& channelId, const std::string& slotId, const std::string& paramId);

// CC value -> parameter value for a mapping (stepped processor params are rounded).
float mappedValue(const MidiMapping& m, int value7, int steps = 0);
// Applies controller moves (the last value per mapping wins). Returns the number of
// parameters that changed.
int applyControls(Project& p, ProjectRuntime* rt, const std::vector<ControlChange>& ccs);

} // namespace roy::midi
