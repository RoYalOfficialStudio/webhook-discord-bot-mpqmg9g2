#pragma once
// WHAT-IF ENGINE: preview a set of proposed changes as B against the original A.
//   begin(steps)  -> applies the proposal (B is active)
//   showA()/showB()  instant switching (the audio graph is rebuilt by the caller's hook)
//   commit()      -> keeps B as ONE undo step
//   discard()     -> back to A, nothing recorded
// Offline comparison renders A and B and reports loudness / band differences.
#include "commands/Commands.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace roy {
class AudioEngine;
}

namespace roy::whatif {

using Steps = std::vector<std::pair<std::string, json>>;

class WhatIfSession {
public:
    WhatIfSession(CommandRegistry& registry, CommandContext& ctx) : reg_(registry), ctx_(ctx) {}
    bool begin(const std::string& name, const Steps& steps);
    bool active() const { return active_; }
    bool showingB() const { return showingB_; }
    void showA();
    void showB();
    void toggle() { showingB_ ? showA() : showB(); }
    bool commit();
    void discard();
    const std::string& error() const { return error_; }

private:
    void setState(const std::string& state);
    CommandRegistry& reg_;
    CommandContext& ctx_;
    std::string name_, stateA_, stateB_, error_;
    bool active_ = false, showingB_ = false;
};

struct ABComparison {
    double lufsA = -70, lufsB = -70;
    double truePeakA = -120, truePeakB = -120;
    std::vector<double> bandDiffDb; // B - A per third-octave band
    std::vector<std::vector<float>> renderA, renderB;
};
// Renders the range for the current project (A) and with `steps` applied (B). The project is left unchanged.
std::optional<ABComparison> compareOffline(AudioEngine& engine, CommandRegistry& registry, CommandContext& ctx, const Steps& steps,
                                           double startBeat, double endBeat, std::string* error = nullptr);

} // namespace roy::whatif
