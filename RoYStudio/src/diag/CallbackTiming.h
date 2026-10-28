#pragma once
// GMB 13.0 - CALLBACK TIMING MONITOR (device-side dropout indicator).
//
// Called from the audio device callback with the steady-clock time and the frame count. It counts
//   * timing gaps: the time since the previous callback is clearly longer than the audio that
//     callback delivered (dt > 1.5 x expected + 2 ms) - the device most probably played silence
//     or repeated audio in between (a real dropout), even though our own processing was in time;
//   * input missing: the device delivered no input buffer although input channels are open;
//   * buffer size changes between callbacks.
// LIMITATION: miniaudio does not report device underruns/overruns (WASAPI shared/exclusive, DirectSound,
// ALSA ...) through its public API, so a gap is an INDICATOR measured from callback timing, not a
// confirmation by the driver. A driver that compensates internally (bigger internal buffers) can
// drop audio without a visible gap; scheduling jitter of the OS can produce a gap without an audible
// dropout when the device has enough buffering. The "CPU over budget" counter of the engine is separate.
//
// Realtime safe: atomics only, no allocation, no locks.
#include <atomic>
#include <cstdint>

namespace roy::diag {

struct CallbackTimingSnapshot {
    uint64_t callbacks = 0;
    uint64_t timingGaps = 0;     // see above
    double maxGapMs = 0;         // largest (dt - expected) seen
    double maxIntervalMs = 0;    // largest time between two callbacks
    uint64_t inputMissing = 0;
    uint64_t sizeChanges = 0;
    uint32_t minFrames = 0, maxFrames = 0;
};

class CallbackTiming {
public:
    void reset() noexcept {
        lastNs_.store(0);
        lastFrames_.store(0);
        callbacks_.store(0);
        gaps_.store(0);
        maxGapUs_.store(0);
        maxIntervalUs_.store(0);
        inputMissing_.store(0);
        sizeChanges_.store(0);
        minFrames_.store(0xFFFFFFFFu);
        maxFrames_.store(0);
    }
    // Audio thread.
    void onCallback(int64_t nowNs, uint32_t frames, double sampleRate, bool inputExpected, bool inputPresent) noexcept {
        constexpr auto rx = std::memory_order_relaxed;
        callbacks_.fetch_add(1, rx);
        if (inputExpected && !inputPresent) inputMissing_.fetch_add(1, rx);
        if (frames < minFrames_.load(rx)) minFrames_.store(frames, rx);
        if (frames > maxFrames_.load(rx)) maxFrames_.store(frames, rx);
        const int64_t last = lastNs_.load(rx);
        const uint32_t lastFrames = lastFrames_.load(rx);
        if (last > 0 && sampleRate > 0 && lastFrames > 0) {
            if (frames != lastFrames) sizeChanges_.fetch_add(1, rx);
            const int64_t dtUs = (nowNs - last) / 1000;
            const int64_t expectedUs = static_cast<int64_t>(1e6 * lastFrames / sampleRate);
            if (dtUs > maxIntervalUs_.load(rx)) maxIntervalUs_.store(dtUs, rx);
            if (dtUs > expectedUs + expectedUs / 2 + 2000) {
                gaps_.fetch_add(1, rx);
                if (dtUs - expectedUs > maxGapUs_.load(rx)) maxGapUs_.store(dtUs - expectedUs, rx);
            }
        }
        lastNs_.store(nowNs, rx);
        lastFrames_.store(frames, rx);
    }
    // Any thread.
    CallbackTimingSnapshot snapshot() const noexcept {
        CallbackTimingSnapshot s;
        s.callbacks = callbacks_.load();
        s.timingGaps = gaps_.load();
        s.maxGapMs = maxGapUs_.load() / 1000.0;
        s.maxIntervalMs = maxIntervalUs_.load() / 1000.0;
        s.inputMissing = inputMissing_.load();
        s.sizeChanges = sizeChanges_.load();
        const uint32_t mn = minFrames_.load();
        s.minFrames = mn == 0xFFFFFFFFu ? 0 : mn;
        s.maxFrames = maxFrames_.load();
        return s;
    }

private:
    std::atomic<int64_t> lastNs_{0}, maxGapUs_{0}, maxIntervalUs_{0};
    std::atomic<uint32_t> lastFrames_{0}, minFrames_{0xFFFFFFFFu}, maxFrames_{0};
    std::atomic<uint64_t> callbacks_{0}, gaps_{0}, inputMissing_{0}, sizeChanges_{0};
};

} // namespace roy::diag
