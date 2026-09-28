#pragma once
// Transport: play / pause / stop / seek / loop / count-in / pre-roll.
//
// Threading: the message thread issues requests (pushed into a wait-free
// queue); the audio thread applies them at the start of each block inside
// advance(), which also splits the block into timeline segments at loop
// boundaries. Position and state are published through atomics.
#include "core/SpscQueue.h"

#include <atomic>
#include <cstdint>

namespace roy {

class Transport {
public:
    enum class State : int { Stopped = 0, Playing = 1, Paused = 2 };

    struct Segment {
        int64_t timelineStart = 0; // timeline sample of the first frame in this segment
        int blockOffset = 0;       // offset inside the audio block
        int numFrames = 0;
        bool rolling = false;      // timeline sources should render
        bool countIn = false;      // only the metronome should sound
        bool jumped = false;       // a discontinuity (loop/seek) happened right before this segment
    };

    Transport();

    // ---- message thread ---------------------------------------------------
    void play();
    void pause();
    void stop();                     // returns to the position where playback started
    void seek(int64_t sample);
    void setLoop(bool enabled, int64_t startSample, int64_t endSample);
    void setCountInSamples(int64_t samples);
    void setPreRollSamples(int64_t samples);

    State state() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }
    bool isPlaying() const { return state() == State::Playing; }
    bool isCountingIn() const { return countingIn_.load(std::memory_order_acquire); }
    int64_t position() const { return position_.load(std::memory_order_acquire); }
    bool loopEnabled() const { return loopEnabled_.load(std::memory_order_acquire); }
    int64_t loopStart() const { return loopStart_.load(std::memory_order_acquire); }
    int64_t loopEnd() const { return loopEnd_.load(std::memory_order_acquire); }
    uint64_t loopCount() const { return loopCount_.load(std::memory_order_acquire); }
    int64_t countInSamples() const { return countInSetting_.load(std::memory_order_acquire); }
    int64_t preRollSamples() const { return preRollSetting_.load(std::memory_order_acquire); }

    // ---- audio thread -----------------------------------------------------
    // Applies pending requests and fills `out` with up to `maxSegments`
    // segments covering `numFrames` frames. Returns the number of segments.
    int advance(int numFrames, Segment* out, int maxSegments) noexcept;

private:
    enum class Req : int { Play, Pause, Stop, Seek, Loop, CountIn, PreRoll };
    struct Request {
        Req type = Req::Play;
        int64_t a = 0, b = 0;
        bool flag = false;
    };
    void post(const Request& r);
    void apply(const Request& r) noexcept;

    SpscQueue<Request> requests_{256};

    // audio-thread state
    int64_t pos_ = 0;
    int64_t playStart_ = 0;
    int64_t countInRemaining_ = 0;
    int64_t countInSamples_ = 0;
    int64_t preRollSamples_ = 0;
    bool pendingJump_ = false;
    State st_ = State::Stopped;
    bool loopOn_ = false;
    int64_t lStart_ = 0, lEnd_ = 0;

    // published
    std::atomic<int> state_{0};
    std::atomic<bool> countingIn_{false};
    std::atomic<int64_t> position_{0};
    std::atomic<bool> loopEnabled_{false};
    std::atomic<int64_t> loopStart_{0}, loopEnd_{0};
    std::atomic<uint64_t> loopCount_{0};
    std::atomic<int64_t> countInSetting_{0}, preRollSetting_{0};
};

} // namespace roy
