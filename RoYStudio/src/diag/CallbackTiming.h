#pragma once
// GMB 13.0 - CALLBACK TIMING MONITOR (device-side dropout indicator).
//
// Called from the audio device callback with the steady-clock time and the frame count.
// TIMING GAP = probable device dropout: the audio clock (frames delivered / sample rate) falls behind
// the wall clock by clearly more than the normal jitter. Bursty delivery (two callbacks back to back,
// then a longer pause - normal for WASAPI shared when RoY's buffer is smaller than the system period)
// does not accumulate and is not counted; a stall that makes the device run out of audio does:
//   lag(t) = (t - t0) - framesDelivered / sampleRate
//   envelope = running max of lag, decaying by 1 ms per second (follows clock drift between the
//              steady clock and the sound card clock, typically < 0.1 ms/s)
//   gap when lag > envelope + max(2 ms, half a buffer) after a 0.5 s warm-up.
// Also counted: callbacks without an input buffer although input is open, buffer size changes.
// LIMITATION: miniaudio does not report device underruns/overruns (WASAPI shared/exclusive, DirectSound,
// ALSA ...) through its public API, so a gap is an INDICATOR measured from callback timing, not a
// confirmation by the driver. A driver that compensates internally (bigger internal buffers) can
// drop audio without a visible gap. The "CPU over budget" counter of the engine is separate.
//
// Realtime safe: atomics only, no allocation, no locks.
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace roy::diag {

struct CallbackTimingSnapshot {
    uint64_t callbacks = 0;
    uint64_t timingGaps = 0;     // see above
    double maxGapMs = 0;         // largest audio-clock deficit beyond the jitter envelope
    double maxIntervalMs = 0;    // largest time between two callbacks
    uint64_t inputMissing = 0;
    uint64_t sizeChanges = 0;
    uint32_t minFrames = 0, maxFrames = 0;
};

class CallbackTiming {
public:
    void reset() noexcept {
        resetRequested_.store(true, std::memory_order_release);
        callbacks_.store(0);
        gaps_.store(0);
        maxGapUs_.store(0);
        maxIntervalUs_.store(0);
        inputMissing_.store(0);
        sizeChanges_.store(0);
        minFrames_.store(0xFFFFFFFFu);
        maxFrames_.store(0);
    }
    // Audio thread only (state below is written by this thread alone).
    void onCallback(int64_t nowNs, uint32_t frames, double sampleRate, bool inputExpected, bool inputPresent) noexcept {
        constexpr auto rx = std::memory_order_relaxed;
        if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
            t0Ns_ = 0;
            lastNs_ = 0;
            lastFrames_ = 0;
        }
        callbacks_.fetch_add(1, rx);
        if (inputExpected && !inputPresent) inputMissing_.fetch_add(1, rx);
        if (frames < minFrames_.load(rx)) minFrames_.store(frames, rx);
        if (frames > maxFrames_.load(rx)) maxFrames_.store(frames, rx);
        if (sampleRate <= 0) return;
        if (t0Ns_ == 0) {
            t0Ns_ = nowNs;
            framesTotal_ = 0;
            envelope_ = 0;
            lastNs_ = nowNs;
            lastFrames_ = frames;
            framesTotal_ += frames;
            return;
        }
        if (frames != lastFrames_) sizeChanges_.fetch_add(1, rx);
        const int64_t dtUs = (nowNs - lastNs_) / 1000;
        if (dtUs > maxIntervalUs_.load(rx)) maxIntervalUs_.store(dtUs, rx);
        const double elapsed = static_cast<double>(nowNs - t0Ns_) * 1e-9;
        const double lag = elapsed - static_cast<double>(framesTotal_) / sampleRate; // before this callback's audio
        const double dt = static_cast<double>(nowNs - lastNs_) * 1e-9;
        envelope_ = std::max(lag, envelope_ - 1e-3 * dt);
        if (elapsed >= 0.5) { // warm-up: the envelope learns the normal jitter first
            const double thr = std::max(2e-3, 0.5 * frames / sampleRate);
            if (lag > prevEnvelope_ + thr) {
                gaps_.fetch_add(1, rx);
                const auto us = static_cast<int64_t>((lag - prevEnvelope_) * 1e6);
                if (us > maxGapUs_.load(rx)) maxGapUs_.store(us, rx);
            }
        }
        prevEnvelope_ = envelope_;
        framesTotal_ += frames;
        lastNs_ = nowNs;
        lastFrames_ = frames;
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
    // audio thread state
    int64_t t0Ns_ = 0, lastNs_ = 0;
    uint64_t framesTotal_ = 0;
    uint32_t lastFrames_ = 0;
    double envelope_ = 0, prevEnvelope_ = 0;
    // shared
    std::atomic<bool> resetRequested_{true};
    std::atomic<int64_t> maxGapUs_{0}, maxIntervalUs_{0};
    std::atomic<uint32_t> minFrames_{0xFFFFFFFFu}, maxFrames_{0};
    std::atomic<uint64_t> callbacks_{0}, gaps_{0}, inputMissing_{0}, sizeChanges_{0};
};

} // namespace roy::diag
