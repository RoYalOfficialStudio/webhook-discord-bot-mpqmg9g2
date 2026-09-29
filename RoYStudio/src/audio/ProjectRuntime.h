#pragma once
// Binds a Project (message-thread model) to the AudioEngine.
// * rebuild(): compiles the project into a new RenderGraph and swaps it in.
// * syncParams(): pushes cheap parameter changes (fader, pan, mute/solo,
//   sends, bypass, plugin params) into the live graph without rebuilding.
// Processor instances and channel parameters persist across rebuilds so that
// effect state (reverb tails, compressor envelopes) is not reset by edits.
#include "audio/AudioEngine.h"
#include "project/Project.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>

namespace roy {

class ProjectRuntime {
public:
    explicit ProjectRuntime(AudioEngine& engine);
    ~ProjectRuntime();

    void setProjectDirectory(const std::filesystem::path& dir) { projectDir_ = dir; }
    const std::filesystem::path& projectDirectory() const { return projectDir_; }

    // Compiles and installs a new graph. Returns false (and keeps the old graph)
    // if the project cannot be compiled. Warnings are collected in lastWarnings().
    bool rebuild(const Project& project);
    void syncParams(const Project& project);
    const std::vector<std::string>& lastWarnings() const { return warnings_; }

    // Asset access (decoded at the engine sample rate, cached).
    std::shared_ptr<const AudioData> asset(const Project& project, const std::string& assetId);
    void addLoadedAsset(const std::string& assetId, std::shared_ptr<const AudioData> data);
    void clearAssetCache();
    // Drops cached audio nothing uses any more: not in the current graph, not referenced by the
    // project (clips incl. raw/tuned, takes, sample rows) and not injected via addLoadedAsset.
    // Such audio is reloaded from its file on demand. Called after every rebuild, so deleted
    // takes/clips and old stretch variants do not accumulate over a long session.
    size_t pruneAssetCache(const Project& project);
    size_t cachedAssetCount() const { return assets_.size() + derivedAssets_.size(); }
    std::filesystem::path resolveAssetPath(const AudioAsset& a) const;

    // Processor instance for a plugin slot (insert or instrument), if any.
    std::shared_ptr<Processor> processorForSlot(const std::string& slotId) const;
    std::shared_ptr<ChannelParams> channelParams(const std::string& channelId) const;
    std::shared_ptr<std::atomic<bool>> monitorFlag(const std::string& trackId) const;

    // Called when a clip needs time-stretch/pitch rendering: returns derived audio.
    using DeriveFn = std::function<std::shared_ptr<AudioData>(const AudioData& src, double stretch, double semitones)>;
    void setDeriveFunction(DeriveFn fn) { derive_ = std::move(fn); }

    // Writes the current processor states back into the project's slots.
    void captureProcessorStates(Project& project) const;
    // Drops the instance of a slot so the next rebuild() creates a fresh one
    // (plugin crash recovery). The old instance lives until its graph is retired.
    void forgetProcessor(const std::string& slotId) { processors_.erase(slotId); }
    // SAFE MODE: third-party plugins (clap:/vst3:) are not loaded; their slots are bypassed
    // and their saved state stays untouched in the project.
    void setSafeMode(bool on) { safeMode_ = on; }
    bool safeMode() const { return safeMode_; }
    // All live processors by slot id (status views).
    std::vector<std::pair<std::string, std::shared_ptr<Processor>>> allProcessors() const;

    int graphLatencySamples() const { return lastLatency_; }
    // Latency with which a channel's output reaches the master mix without compensation.
    int channelArrivalLatency(const std::string& channelId) const {
        auto it = arrival_.find(channelId);
        return it == arrival_.end() ? 0 : it->second;
    }
    AudioEngine& engine() { return engine_; }

    // Live MIDI (keyboard) plays the instrument of this track; re-resolved after every rebuild.
    // Empty / a track without instrument = no live target.
    void setLiveMidiTrack(const Project& project, const std::string& trackId);
    const std::string& liveMidiTrack() const { return liveTrack_; }

private:
    void resolveLiveTarget(const Project& project);
    std::string liveTrack_;
    std::shared_ptr<Processor> ensureProcessor(const Project& project, const PluginSlot& slot, double sr, int maxBlock);
    std::shared_ptr<const AudioData> derived(const Project& p, const AudioClip& c);

    AudioEngine& engine_;
    std::filesystem::path projectDir_;
    std::map<std::string, std::shared_ptr<const AudioData>> assets_;
    std::map<std::string, std::shared_ptr<const AudioData>> derivedAssets_;
    std::set<std::string> pinnedAssets_; // injected data without a file: never evicted
    struct ProcEntry { std::string typeId; std::shared_ptr<Processor> proc; double sr = 0; int block = 0; std::string opaqueState; };
    mutable std::map<std::string, ProcEntry> processors_; // captureProcessorStates refreshes opaqueState
    std::map<std::string, std::shared_ptr<ChannelParams>> params_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> monitors_;
    std::vector<std::string> warnings_;
    DeriveFn derive_;
    uint64_t graphVersion_ = 0;
    int lastLatency_ = 0;
    bool safeMode_ = false;
    std::map<std::string, int> arrival_;
};

// Expands a Beat Lab pattern clip into scheduled note events (implemented in beat/).
struct PatternNote {
    double beat = 0.0;       // absolute timeline beat
    double lengthBeats = 0.1;
    int note = 36;
    float velocity = 0.8f;
    float pan = 0.0f;
    float pitch = 0.0f;
};
std::vector<PatternNote> expandPatternClip(const Pattern& pattern, const PatternClip& clip, uint64_t seed);

} // namespace roy
