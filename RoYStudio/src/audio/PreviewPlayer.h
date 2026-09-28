#pragma once
// Browser audio preview ("audition"). A single stereo voice mixed into the device
// output AFTER the master bus: it never touches project routing, meters, automation
// or exports (offline renders skip it).
//
// play()/stop() run on the message thread; render() on the audio thread
// (no allocation, no locks). Audio data stays alive until the audio thread can no
// longer see it (retired by block counter, like render graphs).
#include "audio/RenderGraph.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace roy {

class PreviewPlayer {
public:
    // data must already be at the engine sample rate.
    void play(std::shared_ptr<const AudioData> data, float gain, bool loop, uint64_t engineBlock);
    void stop();
    void setGain(float g) { gain_.store(g, std::memory_order_relaxed); }
    bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
    // Seconds played of the current preview.
    double positionFrames() const { return static_cast<double>(pos_.load(std::memory_order_relaxed)); }
    // Message thread: frees data that the audio thread cannot reference any more.
    void collect(uint64_t engineBlock);

    // Audio thread: ADDS the preview to out[0..1].
    void render(float* const* out, int numOutputs, int offset, int frames) noexcept;

private:
    std::atomic<const AudioData*> data_{nullptr};
    std::atomic<bool> playing_{false};
    std::atomic<bool> loop_{false};
    std::atomic<float> gain_{0.8f};
    std::atomic<int64_t> pos_{0};
    std::atomic<uint32_t> generation_{0};
    uint32_t seenGeneration_ = 0; // audio thread
    int fadeIn_ = 0;              // audio thread: short fade to avoid clicks
    std::mutex keepMutex_;
    struct Keep { std::shared_ptr<const AudioData> data; uint64_t retireAfter; };
    std::vector<Keep> keep_;
};

} // namespace roy
