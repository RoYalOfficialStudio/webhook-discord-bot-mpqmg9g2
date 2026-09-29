#pragma once
// The realtime audio engine. Owns the transport and metronome, renders the
// current RenderGraph. process() is called by the device callback or by the
// offline renderer - same code path for live playback and export.
#include "audio/Metronome.h"
#include "audio/PreviewPlayer.h"
#include "audio/RenderGraph.h"
#include "audio/Transport.h"
#include "core/SpscQueue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace roy {

// Receives device input on the audio thread (recording, never-lose buffer).
class InputListener {
public:
    virtual ~InputListener() = default;
    virtual void onAudioInput(const float* const* inputs, int numInputs, int numFrames,
                              const Transport::Segment* segments, int numSegments) noexcept = 0;
};

// Offline capture of every channel's post-fader output (analysis, stem export).
// Buffers are preallocated by the caller; the engine only copies into them.
struct ChannelCapture {
    std::vector<std::string> channelIds;             // channels to capture
    std::vector<std::vector<std::vector<float>>> data; // [channel][L/R][frame]
    int64_t writePos = 0;                             // frames written
    int64_t capacity = 0;
    void allocate(const std::vector<std::string>& ids, int64_t frames) {
        channelIds = ids;
        capacity = frames;
        writePos = 0;
        data.assign(ids.size(), std::vector<std::vector<float>>(2, std::vector<float>(static_cast<size_t>(frames), 0.0f)));
    }
};

struct EngineStats {
    double cpuLoad = 0.0;      // last callback time / buffer duration
    double peakCpuLoad = 0.0;
    uint64_t callbacks = 0;
    uint64_t overloads = 0;    // callbacks that took longer than the buffer duration (likely xruns)
    uint64_t nonFiniteFixes = 0; // NaN/Inf samples removed at the output
};

// One raw MIDI channel message from a live input (MIDI keyboard / pad controller).
struct LiveMidiMessage {
    uint8_t status = 0, data1 = 0, data2 = 0;
    uint8_t port = 0; // input device index
    int64_t timeNs = 0; // arrival time (steady clock, ns); 0 = unknown -> start of the next block
};
// A live message received while recording, stamped with the timeline sample of its block.
struct RecordedMidi {
    int64_t timeline = 0;
    LiveMidiMessage msg;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Message thread. Must be called with the device stopped.
    void prepare(double sampleRate, int maxBlockSize);
    double sampleRate() const { return sampleRate_; }
    int maxBlockSize() const { return maxBlock_; }

    Transport& transport() { return transport_; }
    const Transport& transport() const { return transport_; }
    Metronome& metronome() { return metronome_; }
    // Browser audition voice (mixed after the master, never exported).
    PreviewPlayer& preview() { return preview_; }

    // Message thread: installs a new graph. The old one is released later by
    // collectGarbage() once the audio thread no longer uses it.
    void setGraph(std::unique_ptr<RenderGraph> graph);
    const RenderGraph* currentGraphForMessageThread() const { return current_.load(std::memory_order_acquire); }
    void collectGarbage();
    // Message thread, device stopped: returns every processor and delay line of the
    // current graph to its initial state, so an offline render does not depend on
    // what was played before (deterministic exports).
    void resetProcessingState();
    // Offline renders exclude the preview voice.
    void setOfflineRendering(bool on) { offlineRendering_ = on; }
    // True while a callback is running or device is active.
    void setDeviceRunning(bool running) { deviceRunning_.store(running); }
    bool isDeviceRunning() const { return deviceRunning_.load(); }

    void setInputListener(InputListener* l) { listener_.store(l, std::memory_order_release); }
    // Offline only (device stopped): capture channel outputs while rendering.
    void setCapture(ChannelCapture* c) { capture_ = c; }

    // ---- audio thread -------------------------------------------------------
    // Renders `numFrames` frames. `inputs`/`outputs` are non-interleaved.
    void process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numFrames) noexcept;

    EngineStats stats() const;
    void resetStats();

    // Multi-core mixing: number of helper threads besides the audio thread (0 = single-threaded).
    // Message thread, not while a device callback is running. Output is bit-identical for any count.
    void setWorkerThreads(int n);
    int workerThreads() const { return static_cast<int>(workers_.size()); }
    static int defaultWorkerThreads();
    uint64_t processedBlocks() const { return blocksProcessed_.load(std::memory_order_acquire); }

    // ---- live MIDI (MIDI keyboard) ---------------------------------------------
    // Any ONE producer thread (the MIDI input thread). Delivered to the instrument of the
    // live target channel - also while the transport is stopped (MIDI thru). Timing: a message
    // stamped with its arrival time (timeNs) is placed sample-accurately: the previous device
    // callback period is mapped onto the current callback, i.e. constant latency of one
    // callback instead of up to one block of jitter. Unstamped messages start the next block.
    // Sustain pedal (CC64) holds note-offs. Never allocates or locks.
    bool pushLiveMidi(const LiveMidiMessage& m) noexcept;
    // Message thread: channel index in the CURRENT graph that plays live MIDI (-1 = none).
    // Switching the target releases every note on the previous one.
    void setLiveMidiTarget(int channelIndex) { liveTarget_.store(channelIndex, std::memory_order_release); }
    int liveMidiTarget() const { return liveTarget_.load(std::memory_order_acquire); }
    // While on and the transport rolls, live messages are also queued with their timeline sample.
    void setLiveMidiRecording(bool on) { liveRecording_.store(on, std::memory_order_release); }
    bool liveMidiRecording() const { return liveRecording_.load(std::memory_order_acquire); }
    // Message thread: takes the recorded messages collected so far.
    size_t drainRecordedMidi(std::vector<RecordedMidi>& out);
    // Clock used for live MIDI timing (default: std::chrono::steady_clock in ns). TEST hook.
    using ClockFn = int64_t (*)();
    void setLiveMidiClock(ClockFn fn) { clock_ = fn ? fn : &AudioEngine::steadyNowNs; }
    static int64_t steadyNowNs() noexcept;
    uint64_t liveMidiReceived() const { return liveReceived_.load(std::memory_order_relaxed); }
    uint64_t liveMidiDropped() const { return liveDropped_.load(std::memory_order_relaxed); }

private:
    void processChunk(RenderGraph* g, const float* const* inputs, int numInputs, float* const* outputs,
                      int numOutputs, int outOffset, int frames) noexcept;
    void renderSources(GraphChannel& ch, const Transport::Segment& seg) noexcept;
    void runChannel(RenderGraph& g, int index) noexcept;
    void processChannel(RenderGraph& g, GraphChannel& ch, int frames) noexcept;
    void runLevel(RenderGraph& g, int level) noexcept;

    // per-chunk job context (written by the audio thread before a level starts)
    Transport::Segment jobSegs_[8];
    bool jobNotesOff_[8] = {};
    int jobNumSegs_ = 0;
    const float* jobIn_[64] = {};
    int jobNumIn_ = 0;
    int jobFrames_ = 0;

    struct Worker {
        std::thread thread;
        std::atomic<uint32_t> go{0};
        std::atomic<uint32_t> finished{0};
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> quitWorkers_{false};
    uint32_t ticket_ = 0;
    RenderGraph* levelGraph_ = nullptr;
    const int* levelJobs_ = nullptr;
    int levelCount_ = 0;
    void workerLoop(int index, uint32_t startTicket) noexcept;

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    Transport transport_;
    Metronome metronome_;
    PreviewPlayer preview_;
    bool offlineRendering_ = false;
    std::atomic<RenderGraph*> current_{nullptr};
    std::atomic<InputListener*> listener_{nullptr};
    std::atomic<bool> deviceRunning_{false};
    std::atomic<uint64_t> blocksProcessed_{0};
    std::atomic<bool> inProcess_{false};
    // live MIDI
    void collectLiveMidi(bool rolling, int64_t timeline, int outOffset, int frames) noexcept;
    ClockFn clock_ = &AudioEngine::steadyNowNs;
    int64_t cbStartNs_ = 0, prevCbStartNs_ = 0; // current / previous device callback start
    int cbFrames_ = 0;
    LiveMidiMessage pendingLive_{};
    bool hasPendingLive_ = false;               // popped, but due in a later chunk/callback
    std::unique_ptr<SpscQueue<LiveMidiMessage>> liveIn_;
    std::unique_ptr<SpscQueue<RecordedMidi>> liveRec_;
    std::atomic<int> liveTarget_{-1};
    std::atomic<bool> liveRecording_{false};
    std::atomic<uint64_t> liveReceived_{0}, liveDropped_{0};
    static constexpr int kMaxLiveEvents = 256;
    NoteEvent liveEvents_[kMaxLiveEvents];
    int liveCount_ = 0;
    int liveBlockTarget_ = -1;  // target for this block
    int liveOffTarget_ = -1;    // previous target: gets AllNotesOff this block
    int lastLiveTarget_ = -1;
    bool sustain_ = false;
    bool sustained_[128] = {};  // note-offs held by the sustain pedal

    struct Garbage { RenderGraph* graph; uint64_t retireAfter; };
    std::mutex garbageMutex_;
    std::vector<Garbage> garbage_;

    AudioBuffer metronomeBuf_;
    ChannelCapture* capture_ = nullptr;
    bool wasRolling_ = false;

    std::atomic<double> cpuLoad_{0.0}, peakCpuLoad_{0.0};
    std::atomic<uint64_t> overloads_{0}, nonFinite_{0};
};

} // namespace roy
