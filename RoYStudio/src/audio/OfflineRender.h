#pragma once
// Offline (faster than realtime) rendering through the exact same engine code
// path as live playback. Requires the audio device to be stopped.
#include "audio/AudioEngine.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace roy {

struct OfflineRenderOptions {
    int64_t startSample = 0;
    int64_t numFrames = 0;
    int blockSize = 512;
    bool includeMetronome = false;
    int numOutputs = 2;
    // Called every block with progress 0..1; return false to cancel.
    std::function<bool(double)> progress;
};

// Returns planar output channels, or an empty vector (and error) on failure.
std::vector<std::vector<float>> renderOffline(AudioEngine& engine, const OfflineRenderOptions& opt,
                                              std::string* error = nullptr);

} // namespace roy
