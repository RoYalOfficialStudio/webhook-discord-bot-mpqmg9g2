#pragma once
// Immutable render structure consumed by the audio thread.
// Built on the message thread by ProjectRuntime, swapped into the AudioEngine
// atomically, deleted on the message thread once the audio thread has moved on.
#include "audio/Processor.h"
#include "audio/TempoMap.h"
#include "core/AudioBuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace roy {

// Decoded audio at the engine sample rate. Immutable once shared.
struct AudioData {
    std::string assetId;
    double sampleRate = 48000.0;
    int numChannels = 0;
    int64_t numFrames = 0;
    std::vector<std::vector<float>> channels;
    float sample(int ch, int64_t i) const {
        const auto& c = channels[static_cast<size_t>(ch < numChannels ? ch : numChannels - 1)];
        return (i >= 0 && i < numFrames) ? c[static_cast<size_t>(i)] : 0.0f;
    }
};

// Live, lock-free channel parameters and meters. Shared between UI, model sync
// and the audio thread; survives graph rebuilds.
struct ChannelParams {
    static constexpr int kMaxSends = 16;
    std::atomic<float> gainDb{0.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> width{1.0f};
    std::atomic<bool> phaseInvert{false};
    std::atomic<bool> effectiveMute{false}; // mute or muted by solo
    std::atomic<float> sendLevelDb[kMaxSends];
    std::atomic<bool> sendEnabled[kMaxSends];
    std::atomic<bool> insertBypass[32];
    // meters (audio thread writes, UI reads/reset)
    std::atomic<float> peakL{0.0f}, peakR{0.0f};
    std::atomic<float> rmsL{0.0f}, rmsR{0.0f};
    std::atomic<uint32_t> clipCount{0};
    ChannelParams() {
        for (auto& s : sendLevelDb) s.store(-6.0f);
        for (auto& s : sendEnabled) s.store(true);
        for (auto& b : insertBypass) b.store(false);
    }
};

struct ClipPlayback {
    int64_t start = 0;       // timeline sample (inclusive)
    int64_t end = 0;         // timeline sample (exclusive)
    int64_t sourceStart = 0; // source frame played at `start`
    std::shared_ptr<const AudioData> data;
    float gain = 1.0f;
    int64_t fadeIn = 0, fadeOut = 0;
    int fadeInCurve = 1, fadeOutCurve = 1;
    bool reversed = false;
};

struct ScheduledNote {
    int64_t time = 0; // timeline sample
    NoteEvent ev;
};

class DelayLine {
public:
    void setMaxDelay(int samples) { buf_.setSize(2, samples + 1); pos_ = 0; delay_ = 0; }
    void setDelay(int samples) { delay_ = samples; }
    void clear() { buf_.clear(); pos_ = 0; }
    int delay() const { return delay_; }
    // In-place delay of a stereo block. Audio thread safe.
    void process(float* l, float* r, int n) noexcept {
        if (delay_ <= 0) return;
        const int size = buf_.numFrames();
        float* bl = buf_.channel(0);
        float* br = buf_.channel(1);
        for (int i = 0; i < n; ++i) {
            int readPos = pos_ - delay_;
            if (readPos < 0) readPos += size;
            const float ol = bl[readPos], orr = br[readPos];
            bl[pos_] = l[i];
            br[pos_] = r[i];
            l[i] = ol;
            r[i] = orr;
            if (++pos_ >= size) pos_ = 0;
        }
    }
private:
    AudioBuffer buf_;
    int pos_ = 0;
    int delay_ = 0;
};

struct Connection {
    int target = -1;       // index into RenderGraph::channels
    int sendIndex = -1;    // -1 = main output, else index into ChannelParams::send*
    bool preFader = false;
    std::unique_ptr<DelayLine> compensation; // plugin delay compensation
    AudioBuffer scratch;   // for delayed copies
};

struct InsertRef {
    std::shared_ptr<Processor> processor;
    int sidechainChannel = -1; // index into RenderGraph::channels (processed earlier)
    int bypassIndex = 0;
};

// A connection arriving at a channel: outputs[output] of channel `source`.
struct IncomingRef {
    int source = -1;
    int output = -1;
};

struct GraphChannel {
    std::string id;
    int kind = 0; // ChannelKind
    std::shared_ptr<ChannelParams> params;
    std::shared_ptr<Processor> instrument;
    std::vector<InsertRef> inserts;
    std::vector<ClipPlayback> clips;
    std::vector<ScheduledNote> notes; // sorted by time
    std::vector<NoteEvent> eventScratch; // capacity reserved at build time
    int inputLeft = -1, inputRight = -1;
    std::shared_ptr<std::atomic<bool>> monitorEnabled;
    float inputGain = 1.0f;
    std::vector<Connection> outputs; // sends + main out
    std::vector<IncomingRef> incoming; // filled by finalizeGraph(): pulled in source order
    AudioBuffer in;   // summed input (sources + busses)
    AudioBuffer out;  // post-fader output (kept for sidechains)
    AudioBuffer pre;  // pre-fader copy for pre-fader sends
    int chainLatency = 0;
    int inputLatency = 0;
    // smoothing state
    float lastGainL = 0.0f, lastGainR = 0.0f;
    bool gainInitialised = false;
};

struct AutomationTarget {
    int channel = -1;
    enum Kind { Gain, Pan, Width, ProcessorParam, SendLevel } kind = Gain;
    Processor* processor = nullptr;
    int paramIndex = -1; // processor parameter or send index
};

struct AutomationCurve {
    struct Point {
        int64_t sample = 0; // timeline sample
        float value = 0.0f;
        int curve = 0;      // automation::Curve of the segment to the next point
        float tension = 0.0f;
    };
    AutomationTarget target;
    std::vector<Point> points; // sorted by sample
    float valueAt(int64_t t) const noexcept;
};

struct RenderGraph {
    double sampleRate = 48000.0;
    int maxBlock = 512;
    TempoMap tempo;
    std::vector<GraphChannel> channels; // topological order, master last
    int master = -1;
    std::vector<AutomationCurve> automation;
    int totalLatency = 0;
    uint64_t version = 0;
    // Parallel schedule (finalizeGraph): channel indices grouped into dependency levels.
    // Channels of one level never depend on each other and can be processed concurrently.
    std::vector<int> levelOrder;
    std::vector<int> levelStart; // levelStart[k]..levelStart[k+1] indexes levelOrder
};

// Builds `incoming` lists and dependency levels. Message thread (allocates).
void finalizeGraph(RenderGraph& g);

} // namespace roy
