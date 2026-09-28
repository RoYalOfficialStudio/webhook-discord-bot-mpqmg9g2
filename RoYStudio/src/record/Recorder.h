#pragma once
// Recording engine.
//
// Audio thread (onAudioInput): meters, clip detection, never-lose ring buffer,
// and - while recording - pushes samples into lock-free FIFOs.
// Disk thread: drains FIFOs into WAV files. Every take gets its OWN new file
// (unique name, never overwrites). Loop recording starts a new take on every
// loop pass. Punch in/out limits what is written.
//
// NEVER-LOSE: each armed input continuously feeds a bounded ring buffer (last
// N seconds, total memory capped). recoverLastPerformance() writes it to a new
// file, even if Record was never pressed.
#include "audio/AudioEngine.h"
#include "core/SpscQueue.h"
#include "io/AudioFile.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace roy {

struct RecordTrackConfig {
    std::string trackId;
    std::string trackName;
    int inputLeft = 0;
    int inputRight = -1; // -1 = mono
    float inputGainDb = 0.0f;
};

struct FinishedTake {
    std::string trackId;
    std::filesystem::path path;
    int64_t timelineStart = 0; // latency-compensated timeline sample of the first frame
    int64_t frames = 0;
    int loopPass = 0;
    int channels = 1;
    double sampleRate = 48000;
    uint32_t clippedSamples = 0;
    bool diskError = false; // writing failed part-way: the file holds only the first `frames`
};

struct RecoveredPerformance {
    std::string trackId;
    std::filesystem::path path;
    int64_t frames = 0;
    int channels = 1;
    bool hasTimeline = false;   // true if the transport was rolling at the recovered start
    int64_t timelineStart = 0;
    double secondsBeforeNow = 0; // how long ago the recovered audio started
};

class Recorder : public InputListener {
public:
    Recorder();
    ~Recorder() override;

    // ---- configuration (message thread) --------------------------------------
    void prepare(double sampleRate, int maxBlockSize);
    void setTracks(const std::vector<RecordTrackConfig>& tracks);
    void setOutputFolder(const std::filesystem::path& folder) { folder_ = folder; }
    void setFormat(SampleFormat f) { format_ = f; }
    void setPunch(bool enabled, int64_t inSample, int64_t outSample);
    void setLatencyCompensation(int samples) { latency_.store(samples); }
    // Seconds kept per armed input (bounded by kMaxNeverLoseBytes overall).
    void setNeverLoseSeconds(double seconds);
    static constexpr size_t kMaxNeverLoseBytes = 512ull * 1024 * 1024;

    // ---- control -----------------------------------------------------------
    bool startRecording(std::string* error = nullptr); // writing starts when the transport rolls
    void stopRecording();                             // finishes open takes
    bool isRecording() const { return recording_.load(); }
    // Blocks until the disk thread wrote everything queued so far.
    void flush();

    // ---- meters (any thread) --------------------------------------------------
    float inputPeak(const std::string& trackId, bool reset = true);
    uint32_t clipCount(const std::string& trackId) const;
    uint64_t droppedSamples() const { return dropped_.load(); }
    // Disk write failures since startup (disk full, drive removed) and the latest message.
    uint32_t diskErrors() const { return diskErrors_.load(); }
    std::string lastDiskError() const {
        std::lock_guard l(diskErrorMutex_);
        return diskErrorMessage_;
    }

    std::vector<FinishedTake> collectFinishedTakes();

    // ---- never-lose recovery --------------------------------------------------
    std::optional<RecoveredPerformance> recoverLastPerformance(const std::string& trackId, bool trimToPerformance = true,
                                                               std::string* error = nullptr);
    double neverLoseSecondsAvailable(const std::string& trackId) const;

    // ---- InputListener (audio thread) -----------------------------------------
    void onAudioInput(const float* const* inputs, int numInputs, int numFrames, const Transport::Segment* segments,
                      int numSegments) noexcept override;

private:
    struct Marker {
        enum Type : uint8_t { Start, End } type = Start;
        uint64_t samplePos = 0; // position in the FIFO stream (frames) where it applies
        int64_t timeline = 0;
        int loopPass = 0;
    };
    struct TrackState {
        RecordTrackConfig cfg;
        int channels = 1;
        float gain = 1.0f;
        std::unique_ptr<SampleFifo> fifo; // interleaved
        std::unique_ptr<SpscQueue<Marker>> markers;
        uint64_t framesPushed = 0;        // audio thread
        bool takeOpen = false;            // audio thread
        int loopPass = 0;                 // audio thread
        std::atomic<float> peak{0.0f};
        std::atomic<uint32_t> clips{0};
        std::atomic<uint32_t> takeClips{0};
        // never-lose ring
        std::vector<std::vector<float>> ring;
        size_t ringSize = 0;
        std::atomic<uint64_t> ringWritten{0};
        // timeline marks per processed segment: ring position -> timeline sample
        size_t markCount = 0;
        std::unique_ptr<std::atomic<uint64_t>[]> markPos;
        std::unique_ptr<std::atomic<int64_t>[]> markTimeline;
        std::atomic<uint64_t> markWritten{0};
        int takeCounter = 0;
        // disk thread
        WavWriter writer;
        uint64_t framesDrained = 0;
        FinishedTake current;
        bool writing = false;
        Marker pendingMarker;
        bool hasPendingMarker = false;
    };
    struct Config {
        std::vector<std::unique_ptr<TrackState>> tracks;
    };

    void diskLoop();
    void drain(Config& cfg, bool final);
    TrackState* find(const std::string& trackId) const;
    void retire(std::unique_ptr<Config> old);

    double sr_ = 48000;
    int maxBlock_ = 512;
    std::filesystem::path folder_;
    SampleFormat format_ = SampleFormat::Pcm24;
    double neverLoseSeconds_ = 120.0;

    std::atomic<Config*> config_{nullptr};
    std::unique_ptr<Config> owned_;
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<bool> inCallback_{false};
    std::atomic<bool> forceClose_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> punchOn_{false};
    std::atomic<int64_t> punchIn_{0}, punchOut_{0};
    std::atomic<int> latency_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint32_t> diskErrors_{0};
    mutable std::mutex diskErrorMutex_;
    std::string diskErrorMessage_;

    std::mutex diskMutex_;
    std::condition_variable diskCv_;
    std::thread diskThread_;
    std::atomic<bool> quit_{false};
    std::mutex finishedMutex_;
    std::vector<FinishedTake> finished_;
    std::atomic<uint64_t> flushRequest_{0}, flushDone_{0};
};

} // namespace roy
