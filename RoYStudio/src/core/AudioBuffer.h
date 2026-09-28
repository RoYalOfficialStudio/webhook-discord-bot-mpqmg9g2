#pragma once
// Non-interleaved multichannel float buffers.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

namespace roy {

// Non-owning view of `numChannels` channel pointers, each `numFrames` long.
struct AudioBlock {
    float* const* channels = nullptr;
    int numChannels = 0;
    int numFrames = 0;

    float* channel(int c) const { return channels[c]; }
    void clear() const {
        for (int c = 0; c < numChannels; ++c) std::memset(channels[c], 0, sizeof(float) * static_cast<size_t>(numFrames));
    }
    void applyGain(float g) const {
        for (int c = 0; c < numChannels; ++c)
            for (int i = 0; i < numFrames; ++i) channels[c][i] *= g;
    }
};

class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(int numChannels, int numFrames) { setSize(numChannels, numFrames); }

    // Allocates. Never call on the audio thread.
    void setSize(int numChannels, int numFrames) {
        numChannels_ = numChannels;
        numFrames_ = numFrames;
        data_.assign(static_cast<size_t>(numChannels) * static_cast<size_t>(numFrames), 0.0f);
        ptrs_.resize(static_cast<size_t>(numChannels));
        for (int c = 0; c < numChannels; ++c) ptrs_[static_cast<size_t>(c)] = data_.data() + static_cast<size_t>(c) * static_cast<size_t>(numFrames);
    }
    int numChannels() const { return numChannels_; }
    int numFrames() const { return numFrames_; }
    float* channel(int c) { return ptrs_[static_cast<size_t>(c)]; }
    const float* channel(int c) const { return ptrs_[static_cast<size_t>(c)]; }
    float* const* channels() { return ptrs_.data(); }
    void clear() { std::fill(data_.begin(), data_.end(), 0.0f); }

    AudioBlock block() { return AudioBlock{ptrs_.data(), numChannels_, numFrames_}; }
    // A view onto the first `frames` frames (frames <= numFrames). Uses a
    // caller-provided pointer array so it never allocates.
    AudioBlock view(float** scratchPtrs, int offset, int frames) {
        for (int c = 0; c < numChannels_; ++c) scratchPtrs[c] = ptrs_[static_cast<size_t>(c)] + offset;
        return AudioBlock{scratchPtrs, numChannels_, frames};
    }

private:
    int numChannels_ = 0;
    int numFrames_ = 0;
    std::vector<float> data_;
    std::vector<float*> ptrs_;
};

} // namespace roy
