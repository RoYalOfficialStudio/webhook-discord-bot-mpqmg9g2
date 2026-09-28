#pragma once
// DAW side of the plugin sandbox:
//
//   RoYStudio -> PluginHostManager -> RoYPluginHost.exe (one process per instance) -> plugin
//
// A SandboxedPluginProcessor is a normal Processor in the mixer graph. Audio goes
// through shared memory, control through the host's stdin/stdout. If the host
// process crashes or hangs, the processor turns itself into a pass-through
// (effects) / silence (instruments), "PLUGIN CRASHED" is logged and a crash event
// is queued; the project keeps playing. RestartPlugin recreates the instance from
// the last known state.
#include "audio/Processor.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace roy {

namespace plugins {

struct CrashEvent {
    std::string typeId;
    std::string pluginName;
    std::string reason;   // "crashed (signal 11)", "hung (no response in 250 ms)", ...
    std::string time;     // ISO 8601
};

// Global plugin host settings.
void setHostExecutable(const std::string& path); // default: <exe dir>/roy_plugin_host(.exe)
std::string hostExecutable();
void setCrashReportDirectory(const std::string& dir); // JSON crash reports are written here if set
// Max time the audio thread waits for the sandbox per block before declaring it hung.
void setProcessTimeoutMs(int ms);
int processTimeoutMs();

// Crash events since the last call (message thread).
std::vector<CrashEvent> takeCrashEvents();

// "clap:<module path>|<plugin id>"
std::string makeClapTypeId(const std::string& modulePath, const std::string& pluginId);
bool parseClapTypeId(const std::string& typeId, std::string& modulePath, std::string& pluginId);

class Sandbox; // host process + shared memory (Sandbox.cpp)

} // namespace plugins

class SandboxedPluginProcessor final : public Processor {
public:
    // Launches the host and instantiates the plugin. Returns nullptr on failure (error set).
    static std::unique_ptr<SandboxedPluginProcessor> create(const std::string& typeId, std::string* error = nullptr);
    ~SandboxedPluginProcessor() override;

    std::string typeId() const override { return typeId_; }
    std::string displayName() const override { return name_; }
    bool isInstrument() const override { return instrument_; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock* sidechain, const NoteEvent* events, int numEvents) noexcept override;
    int latencySamples() const override { return latency_.load(); }
    void setHostTempo(double bpm) override { tempo_.store(bpm); }
    json saveState() const override;
    void loadState(const json& state) override;

    bool alive() const;
    // Empty when healthy, else "crashed (...)" / "hung (...)".
    std::string problem() const;
    int hostPid() const;
    // For tests: simulate a hang/crash of the host.
    void killHostForTest();

private:
    SandboxedPluginProcessor(std::vector<ParamInfo> params);
    std::string typeId_, name_;
    bool instrument_ = false;
    std::unique_ptr<plugins::Sandbox> box_;
    std::vector<uint32_t> clapIds_;
    std::unique_ptr<float[]> lastSent_;
    std::atomic<int> latency_{0};
    std::atomic<double> tempo_{120.0};
    std::atomic<bool> activated_{false};
    uint32_t seq_ = 0;
    int chunk_ = 512;
    mutable std::mutex stateMutex_;
    mutable json lastState_ = json::object(); // last good state, used after a crash
};

// Registers the "clap:" prefix with the ProcessorFactory. Idempotent.
void registerPluginProcessors();

} // namespace roy
