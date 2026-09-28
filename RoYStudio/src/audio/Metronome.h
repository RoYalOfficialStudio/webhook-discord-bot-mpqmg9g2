#pragma once
// Sample-accurate metronome. Click sounds are synthesised once in prepare()
// (message thread); the audio thread only copies samples.
#include "audio/TempoMap.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace roy {

class Metronome {
public:
    void prepare(double sampleRate);
    void setEnabled(bool e) { enabled_.store(e); }
    bool enabled() const { return enabled_.load(); }
    void setGainDb(float db) { gainDb_.store(db); }
    void setCountInOnly(bool v) { countInOnly_.store(v); }

    // Adds clicks for the timeline range [start, start+frames) into out (stereo).
    // `force` renders clicks even if disabled (used for count-in).
    void render(const TempoMap& tempo, int64_t timelineStart, int frames, float* left, float* right,
                bool countIn) noexcept;
    void reset() noexcept { playing_ = -1; }

    // Number of clicks triggered since prepare() (for tests).
    uint64_t clicksTriggered() const { return clicks_.load(); }

private:
    double sr_ = 48000.0;
    std::vector<float> accent_, normal_;
    int playing_ = -1; // 0 = accent, 1 = normal
    size_t playPos_ = 0;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> countInOnly_{false};
    std::atomic<float> gainDb_{-6.0f};
    std::atomic<uint64_t> clicks_{0};
};

} // namespace roy
