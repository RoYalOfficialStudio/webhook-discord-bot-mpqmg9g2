#pragma once
// Audio device management on top of miniaudio (WASAPI / DirectSound on
// Windows, ALSA / PulseAudio / JACK on Linux, Core Audio on macOS, plus a
// "null" backend for headless tests).
#include "audio/AudioEngine.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace roy {

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
    bool isInput = false;
    int index = 0;
};

struct AudioDeviceConfig {
    std::string backend = "auto"; // auto | wasapi | dsound | winmm | alsa | pulseaudio | jack | coreaudio | null
    std::string outputDevice;     // empty = system default
    std::string inputDevice;      // empty = system default
    double sampleRate = 48000.0;
    int bufferSize = 256;
    int outputChannels = 2;
    int inputChannels = 2;
    bool enableInput = true;
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    static const std::vector<double>& supportedSampleRates();
    static const std::vector<int>& supportedBufferSizes();

    bool initialise(const std::string& backend = "auto", std::string* error = nullptr);
    std::string backendName() const;
    std::vector<AudioDeviceInfo> outputDevices() const;
    std::vector<AudioDeviceInfo> inputDevices() const;

    // Opens the device and prepares the engine for its sample rate / buffer size.
    bool open(const AudioDeviceConfig& cfg, AudioEngine& engine, std::string* error = nullptr);
    bool start(std::string* error = nullptr);
    void stop();
    void close();
    bool isOpen() const;
    bool isRunning() const;

    double actualSampleRate() const { return actualRate_; }
    int actualBufferSize() const { return actualBuffer_; }
    int actualInputChannels() const { return inChannels_; }
    int actualOutputChannels() const { return outChannels_; }
    // Round-trip latency estimate in samples (input + output buffering).
    int latencySamples() const;
    uint64_t callbackCount() const { return callbacks_.load(); }

    // ---- device loss / reconnect (message thread) -----------------------------
    // A device that stops without stop() being called (unplugged, taken exclusively
    // by another app, driver reset) or whose callbacks stall for kStallSeconds is
    // "lost". poll() reports it once, then retries opening it with backoff; when
    // the named device stays gone it falls back to the system default device.
    enum class Health { Ok, Lost, StillLost, Reconnected };
    Health poll(double nowSeconds);
    bool deviceLost() const { return lostHandled_; }
    std::string lostReason() const;
    int reconnectAttempts() const { return reconnectAttempts_; }
    const AudioDeviceConfig& config() const { return cfg_; }
    static constexpr double kStallSeconds = 3.0;
    // FAULT INJECTION (tests only): stops the device behind the manager's back,
    // which is exactly what the backend does when the device disappears.
    void simulateDeviceLoss();
    // FAULT INJECTION (tests only): reconnect attempts fail while this is set.
    void simulateDeviceAbsent(bool absent) { simulateAbsent_ = absent; }
    // FAULT INJECTION (tests only): the callback outputs silence without processing,
    // like a driver that stopped delivering buffers.
    void simulateCallbackStall(bool stall) { stallForTest_.store(stall); }

private:
    struct Impl;
    static void dataCallback(void* device, void* output, const void* input, unsigned int frameCount);
    static void notificationCallback(const void* notification);
    void markLost(const char* reason);
    std::unique_ptr<Impl> impl_;
    AudioEngine* engine_ = nullptr;
    double actualRate_ = 0;
    int actualBuffer_ = 0;
    int inChannels_ = 0, outChannels_ = 0;
    std::atomic<uint64_t> callbacks_{0};
    // loss detection
    AudioDeviceConfig cfg_;
    AudioEngine* lostEngine_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> lost_{false};
    std::atomic<bool> stallForTest_{false};
    std::atomic<const char*> lostReason_{nullptr};
    bool lostHandled_ = false;
    bool simulateAbsent_ = false;
    int reconnectAttempts_ = 0;
    double nextRetry_ = 0;
    uint64_t lastCallbacks_ = 0;
    double lastProgress_ = -1;
    AudioBuffer inPlanar_, outPlanar_;
};

} // namespace roy
