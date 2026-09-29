#pragma once
// Multi-resolution min/max peak cache for fast waveform drawing.
// Levels: 64, 256, 1024, 4096, 16384, 65536 samples per peak. Drawing any
// zoom level touches at most ~2 peaks per pixel, so huge projects stay fast.
// Caches are persisted to <project>/Cache/<assetId>.roypk and validated
// against the asset's frame count / sample rate.
#include "audio/RenderGraph.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace roy {

class WaveformCache {
public:
    static constexpr int kBaseSamplesPerPeak = 64;
    static constexpr int kLevelFactor = 4;
    static constexpr int kNumLevels = 6;

    void build(const AudioData& data);
    // Fills `pixels` min/max pairs for channel `ch` over [startSample, endSample).
    void getPeaks(int ch, double startSample, double endSample, int pixels, float* mins, float* maxs) const;
    bool save(const std::filesystem::path& file) const;
    bool load(const std::filesystem::path& file, int64_t expectFrames, double expectRate);

    int numChannels() const { return numChannels_; }
    int64_t numFrames() const { return numFrames_; }
    size_t memoryBytes() const;

private:
    struct Level {
        int samplesPerPeak = 0;
        std::vector<std::vector<float>> mins, maxs; // [channel][peak]
    };
    int numChannels_ = 0;
    int64_t numFrames_ = 0;
    double sampleRate_ = 0;
    std::vector<Level> levels_;
};

// Per-project store of waveform caches.
class WaveformStore {
public:
    explicit WaveformStore(std::filesystem::path cacheDir = {}) : dir_(std::move(cacheDir)) {}
    void setCacheDirectory(const std::filesystem::path& d) { dir_ = d; }
    // Returns a cache for the asset, loading from disk or building (and saving) it.
    std::shared_ptr<const WaveformCache> get(const std::string& assetId, const AudioData& data);
    void invalidate(const std::string& assetId);
    // Drops in-memory caches of assets not in `keep` (the files on disk stay as a cache).
    size_t retainOnly(const std::set<std::string>& keep);
    size_t size() const { return caches_.size(); }
    int builtCount() const { return built_; }
    int loadedCount() const { return loaded_; }

private:
    std::filesystem::path dir_;
    std::map<std::string, std::shared_ptr<WaveformCache>> caches_;
    int built_ = 0, loaded_ = 0;
};

} // namespace roy
