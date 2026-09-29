#pragma once
// FIRST REAL MUSIC SESSION: a guided test through the whole workflow on a real machine -
// beat (drums + 808) -> vocal recording -> pitch editing -> mix -> export -> plugin check.
// Every step can be done by the tester by hand, or with its "DO IT" button, which runs the same
// ordinary (undoable) commands. Each result (PASS / FAIL + reason) is appended to
// <user data>/FirstRealMusicSession_results.md, which the diagnostic package includes.
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace roy::gui {

class App;

struct SessionState {
    std::string drums, pattern, patternClip, bass, bassClip, vocal, vocalCh, vocalClip, fxBus;
    std::string vst3Slot, clapSlot;
    float vst3Expected = -1.0f, clapExpected = -1.0f;
    double recordStartSec = -1.0;
    float maxInput = 0.0f;                    // loudest input seen while the input step is open
    std::map<size_t, std::string> results;    // step index -> "PASS ..." / "FAIL ..."
};

struct SessionStep {
    std::string group;  // "A  BEAT", ...
    std::string title;
    std::string hint;   // what to look at / do by hand
    // Runs the step. Returns "" on success or the reason it failed; `info` may carry details.
    std::function<std::string(App&, SessionState&, std::string& info)> run;
};

const std::vector<SessionStep>& musicSessionSteps();
// Runs step i, stores the result in the state and in the results file. Returns true on success.
bool runSessionStep(App& app, SessionState& st, size_t i);
std::string sessionResultsFile();
// Automated run of every step (development / Wine / CI): recording is pumped in real time,
// steps that need a person (microphone level, opening Explorer) are SKIPPED. Writes
// <folder>/session_test_report.md; true if no step FAILED.
bool runSessionSelfTest(App& app, const std::string& folder);

} // namespace roy::gui
