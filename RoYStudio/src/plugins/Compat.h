#pragma once
// PLUGIN COMPATIBILITY CHECK: runs every scanned plugin through the whole RoY lifecycle in the
// sandbox - load -> audio (effects: test tone, instruments: a note) -> parameters -> state
// save/restore into a fresh instance -> editor open/close (optional) -> unload - and reports
// what worked. Used by `roy_cli plugin-compat` for installed third-party plugins.
#include "plugins/Scanner.h"

#include <string>
#include <vector>

namespace roy::plugins {

struct CompatResult {
    std::string typeId, name, vendor, format, category;
    int params = 0;
    int latency = 0;
    bool loaded = false;
    bool audioOk = false;     // finite output, host process still alive
    bool audible = false;     // output above -80 dBFS
    bool stateOk = false;     // parameters identical after save -> load into a new instance
    bool editorChecked = false, editorOk = false, hasEditor = false;
    bool unloaded = false;    // host process ended after the instance was destroyed
    double outRmsDb = -200;
    double loadMs = 0;
    std::vector<std::string> notes;
    bool pass() const { return loaded && audioOk && stateOk && unloaded && (!editorChecked || editorOk || !hasEditor); }
};

struct CompatOptions {
    bool editor = false;      // open/close editors (needs a display)
    double seconds = 1.0;     // audio per plugin
    double sampleRate = 48000.0;
    int blockSize = 512;
};

CompatResult checkPlugin(const PluginRecord& rec, const CompatOptions& opt);
std::vector<CompatResult> checkAll(const PluginDatabase& db, const CompatOptions& opt);
std::string compatReportMarkdown(const std::vector<CompatResult>& results, const CompatOptions& opt);

} // namespace roy::plugins
