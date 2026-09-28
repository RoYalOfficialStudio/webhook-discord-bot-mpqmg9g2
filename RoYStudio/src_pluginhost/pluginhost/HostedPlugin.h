#pragma once
// Format-independent plugin instance inside RoYPluginHost (CLAP or VST3).
// Main thread: everything except process(). process() runs on the host's audio thread.
#include "plugins/PluginProtocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace roy::pluginhost {

using json = nlohmann::json;

class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;

    // {format,id,name,vendor,version,instrument,params:[{id,name,min,max,default,value,stepped,steps,hidden,readOnly,bypass}],
    //  inputChannels,outputChannels,hasState,hasEditor}
    virtual json info() = 0;
    virtual bool activate(double sampleRate, uint32_t maxFrames) = 0;
    virtual uint32_t latency() = 0;
    virtual void reset() = 0;
    virtual bool saveState(std::vector<uint8_t>& out) = 0;
    virtual bool loadState(const std::vector<uint8_t>& in) = 0;
    virtual json paramValues() = 0;

    // Audio thread: reads b.in/events/params, writes b.out and b.outParams/numOutParams.
    // Returns > 0 on success, 0 on error (the DAW then bypasses the block).
    virtual int32_t process(pluginipc::Block& b) noexcept = 0;

    // Main thread housekeeping (GUI events, timers, callbacks).
    virtual void idle() = 0;
    virtual bool editorSupported() = 0;
    virtual bool openEditor(bool alwaysOnTop, std::string* error) = 0;
    virtual void closeEditor() = 0;
    virtual json editorState() = 0;
    // Deactivate and destroy the instance (before the module is unloaded).
    virtual void shutdown() = 0;

protected:
    // Parameter edits made in the plugin's own GUI (main thread) waiting for the audio thread.
    struct Edit { uint32_t id; double value; };
    std::mutex editMutex_;
    std::vector<Edit> guiEdits_;
    void pushGuiEdit(uint32_t id, double v) {
        std::lock_guard<std::mutex> lk(editMutex_);
        if (guiEdits_.size() < 1024) guiEdits_.push_back({id, v});
    }
    // Audio thread: take pending edits without blocking (try_lock), into a fixed buffer.
    int takeGuiEdits(Edit* dst, int max) noexcept {
        std::unique_lock<std::mutex> lk(editMutex_, std::try_to_lock);
        if (!lk.owns_lock()) return 0;
        int n = 0;
        for (auto& e : guiEdits_) {
            if (n >= max) break;
            dst[n++] = e;
        }
        guiEdits_.clear();
        return n;
    }
};

} // namespace roy::pluginhost
