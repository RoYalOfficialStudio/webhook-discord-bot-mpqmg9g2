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

private:
    struct Impl;
    static void dataCallback(void* device, void* output, const void* input, unsigned int frameCount);
    std::unique_ptr<Impl> impl_;
    AudioEngine* engine_ = nullptr;
    double actualRate_ = 0;
    int actualBuffer_ = 0;
    int inChannels_ = 0, outChannels_ = 0;
    std::atomic<uint64_t> callbacks_{0};
    AudioBuffer inPlanar_, outPlanar_;
};

} // namespace roy
