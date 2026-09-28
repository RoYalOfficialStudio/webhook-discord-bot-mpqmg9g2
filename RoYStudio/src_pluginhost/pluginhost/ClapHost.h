#pragma once
// In-process CLAP loader. Used ONLY inside the sandbox process (RoYPluginHost):
// the DAW itself never loads third-party plugin code into its own address space.
#include "pluginhost/EditorWindow.h"
#include "pluginhost/HostedPlugin.h"

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace roy::clap {

using json = nlohmann::json;

// A loaded .clap module (shared library + initialised entry).
class Module {
public:
    ~Module();
    static std::unique_ptr<Module> load(const std::string& path, std::string* error);
    const clap_plugin_factory_t* factory() const { return factory_; }
    const std::string& path() const { return path_; }
    json describeAll() const;

private:
    Module() = default;
    void* lib_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_factory_t* factory_ = nullptr;
    std::string path_;
    bool initialised_ = false;
};

json describe(const clap_plugin_descriptor_t* d);
bool isInstrument(const clap_plugin_descriptor_t* d);

struct ParamDesc {
    clap_id id;
    std::string name;
    double minValue, maxValue, defaultValue;
    uint32_t flags;
};

// One CLAP plugin instance with an honest minimal clap_host (gui, timer-support,
// posix-fd-support, params, latency, state) implementing HostedPlugin.
class Instance final : public pluginhost::HostedPlugin {
public:
    ~Instance() override;
    static std::unique_ptr<Instance> create(Module& m, const std::string& pluginId, std::string* error);

    const clap_plugin_t* plugin() const { return plugin_; }
    bool instrument() const { return instrument_; }
    int inputChannels() const { return inChannels_; }
    int outputChannels() const { return outChannels_; }
    const std::vector<ParamDesc>& params() const { return params_; }

    json info() override;
    bool activate(double sampleRate, uint32_t maxFrames) override;
    uint32_t latency() override;
    void reset() override;
    bool saveState(std::vector<uint8_t>& out) override;
    bool loadState(const std::vector<uint8_t>& in) override;
    json paramValues() override;
    int32_t process(pluginipc::Block& b) noexcept override;
    void idle() override;
    bool editorSupported() override;
    bool openEditor(bool alwaysOnTop, std::string* error) override;
    void closeEditor() override;
    json editorState() override;
    void shutdown() override;

    double paramValue(clap_id id) const;

private:
    Instance() = default;
    void deactivate();
    static const void* hostGetExtension(const clap_host_t*, const char*);
    static void hostRequestRestart(const clap_host_t*);
    static void hostRequestProcess(const clap_host_t*);
    static void hostRequestCallback(const clap_host_t*);

    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* stateExt_ = nullptr;
    const clap_plugin_latency_t* latencyExt_ = nullptr;
    const clap_plugin_gui_t* guiExt_ = nullptr;
    const clap_plugin_timer_support_t* timerExt_ = nullptr;
    const clap_plugin_posix_fd_support_t* fdExt_ = nullptr;
    std::vector<ParamDesc> params_;
    bool instrument_ = false;
    int inChannels_ = 0, outChannels_ = 2;
    bool active_ = false;
    bool processing_ = false;
    std::atomic<bool> callbackRequested_{false};
    std::atomic<bool> restartRequested_{false};
    int64_t steady_ = 0;
    double sampleRate_ = 48000;
    uint32_t maxFrames_ = 512;

    // GUI
    pluginhost::EditorWindow window_;
    pluginhost::RunLoop runLoop_;
    bool guiCreated_ = false;
    bool closeRequested_ = false;
    std::vector<std::pair<clap_id, int>> timerIds_; // clap timer id -> runloop id
    std::vector<std::pair<int, int>> fdIds_;        // fd -> runloop id
    friend struct HostExt;

    // audio-thread scratch (preallocated)
    struct EventStore;
    std::unique_ptr<EventStore> events_;
};

} // namespace roy::clap
