#pragma once
// In-process CLAP loader. Used ONLY inside the sandbox process (RoYPluginHost):
// the DAW itself never loads third-party plugin code into its own address space.
#include <clap/clap.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
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
    // Descriptors of all plugins in the module (JSON, see describe()).
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

// One plugin instance with a minimal, honest clap_host implementation.
class Instance {
public:
    ~Instance();
    static std::unique_ptr<Instance> create(Module& m, const std::string& pluginId, std::string* error);

    const clap_plugin_t* plugin() const { return plugin_; }
    const clap_plugin_descriptor_t* descriptor() const { return plugin_->desc; }
    bool instrument() const { return instrument_; }
    int inputChannels() const { return inChannels_; }
    int outputChannels() const { return outChannels_; }
    const std::vector<ParamDesc>& params() const { return params_; }
    json info() const;

    // Main thread.
    bool activate(double sampleRate, uint32_t maxFrames);
    void deactivate();
    bool active() const { return active_; }
    uint32_t latency() const;
    // Clears the plugin's internal buffers/voices (host guarantees the audio thread is idle).
    void reset();
    double paramValue(clap_id id) const;
    bool saveState(std::vector<uint8_t>& out);
    bool loadState(const std::vector<uint8_t>& in);
    // Runs pending on_main_thread callbacks requested by the plugin.
    void idle();

    // Audio thread. `in`/`out` are 2 channels each (in may be null for instruments).
    int32_t process(float* const* in, float* const* out, uint32_t frames, const clap_input_events_t* events,
                    const clap_event_transport_t* transport) noexcept;
    void stopProcessingIfStarted() noexcept;

private:
    Instance() = default;
    static const void* hostGetExtension(const clap_host_t*, const char*);
    static void hostRequestRestart(const clap_host_t*);
    static void hostRequestProcess(const clap_host_t*);
    static void hostRequestCallback(const clap_host_t*);

    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* stateExt_ = nullptr;
    const clap_plugin_latency_t* latencyExt_ = nullptr;
    std::vector<ParamDesc> params_;
    bool instrument_ = false;
    int inChannels_ = 0, outChannels_ = 2;
    bool active_ = false;
    bool processing_ = false;
    std::atomic<bool> callbackRequested_{false};
    int64_t steady_ = 0;
};

} // namespace roy::clap
