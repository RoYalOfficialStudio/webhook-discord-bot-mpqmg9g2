#include "audio/DeviceManager.h"
#include "core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace roy {

struct DeviceManager::Impl {
    ma_context context{};
    bool contextOk = false;
    ma_device device{};
    bool deviceOk = false;
    std::vector<ma_device_info> playback, capture;
};

DeviceManager::DeviceManager() : impl_(std::make_unique<Impl>()) {}

DeviceManager::~DeviceManager() {
    close();
    if (impl_->contextOk) ma_context_uninit(&impl_->context);
}

const std::vector<double>& DeviceManager::supportedSampleRates() {
    static const std::vector<double> r = {44100.0, 48000.0, 88200.0, 96000.0};
    return r;
}

const std::vector<int>& DeviceManager::supportedBufferSizes() {
    static const std::vector<int> b = {32, 64, 128, 256, 512, 1024};
    return b;
}

namespace {
bool backendFromName(const std::string& n, ma_backend& out) {
    struct { const char* name; ma_backend b; } table[] = {
        {"wasapi", ma_backend_wasapi}, {"dsound", ma_backend_dsound}, {"winmm", ma_backend_winmm},
        {"coreaudio", ma_backend_coreaudio}, {"alsa", ma_backend_alsa}, {"pulseaudio", ma_backend_pulseaudio},
        {"jack", ma_backend_jack}, {"null", ma_backend_null}};
    for (auto& e : table)
        if (n == e.name) {
            out = e.b;
            return true;
        }
    return false;
}
} // namespace

bool DeviceManager::initialise(const std::string& backend, std::string* error) {
    close();
    if (impl_->contextOk) {
        ma_context_uninit(&impl_->context);
        impl_->contextOk = false;
    }
    ma_result r;
    if (backend == "auto" || backend.empty()) {
        r = ma_context_init(nullptr, 0, nullptr, &impl_->context);
    } else {
        ma_backend b;
        if (!backendFromName(backend, b)) {
            if (error) *error = "unknown audio backend: " + backend;
            return false;
        }
        r = ma_context_init(&b, 1, nullptr, &impl_->context);
    }
    if (r != MA_SUCCESS) {
        if (error) *error = std::string("audio context init failed: ") + ma_result_description(r);
        log::error("device", "context init failed for backend '{}': {}", backend, ma_result_description(r));
        return false;
    }
    impl_->contextOk = true;
    ma_device_info *pb = nullptr, *cap = nullptr;
    ma_uint32 npb = 0, ncap = 0;
    if (ma_context_get_devices(&impl_->context, &pb, &npb, &cap, &ncap) == MA_SUCCESS) {
        impl_->playback.assign(pb, pb + npb);
        impl_->capture.assign(cap, cap + ncap);
    }
    log::info("device", "audio backend '{}' ready: {} outputs, {} inputs", backendName(), npb, ncap);
    return true;
}

std::string DeviceManager::backendName() const {
    if (!impl_->contextOk) return "none";
    return ma_get_backend_name(impl_->context.backend);
}

std::vector<AudioDeviceInfo> DeviceManager::outputDevices() const {
    std::vector<AudioDeviceInfo> v;
    for (size_t i = 0; i < impl_->playback.size(); ++i)
        v.push_back({impl_->playback[i].name, impl_->playback[i].isDefault != 0, false, static_cast<int>(i)});
    return v;
}

std::vector<double> DeviceManager::nativeSampleRates(const std::string& deviceName, bool input) const {
    std::vector<double> out;
    if (!impl_->contextOk) return out;
    const auto& list = input ? impl_->capture : impl_->playback;
    const ma_device_id* id = nullptr;
    for (auto& d : list)
        if (deviceName.empty() ? d.isDefault != 0 : deviceName == d.name) id = &d.id;
    ma_device_info info{};
    if (ma_context_get_device_info(&impl_->context, input ? ma_device_type_capture : ma_device_type_playback, id, &info) != MA_SUCCESS) return out;
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
        const double sr = info.nativeDataFormats[i].sampleRate;
        if (sr > 0 && std::find(out.begin(), out.end(), sr) == out.end()) out.push_back(sr);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<AudioDeviceInfo> DeviceManager::inputDevices() const {
    std::vector<AudioDeviceInfo> v;
    for (size_t i = 0; i < impl_->capture.size(); ++i)
        v.push_back({impl_->capture[i].name, impl_->capture[i].isDefault != 0, true, static_cast<int>(i)});
    return v;
}

void DeviceManager::dataCallback(void* devicePtr, void* output, const void* input, unsigned int frameCount) {
    auto* device = static_cast<ma_device*>(devicePtr);
    auto* self = static_cast<DeviceManager*>(device->pUserData);
    if (!self || !self->engine_) return;
    if (self->stallForTest_.load(std::memory_order_relaxed)) {
        std::memset(output, 0, sizeof(float) * frameCount * static_cast<size_t>(self->outChannels_));
        return;
    }
    self->callbacks_.fetch_add(1, std::memory_order_relaxed);
    const int inCh = self->inChannels_;
    const int outCh = self->outChannels_;
    const int cap = self->outPlanar_.numFrames();
    const float* in = static_cast<const float*>(input);
    float* out = static_cast<float*>(output);
    unsigned int done = 0;
    while (done < frameCount) {
        const int n = static_cast<int>(std::min<unsigned int>(frameCount - done, static_cast<unsigned int>(cap)));
        for (int c = 0; c < inCh; ++c) {
            float* dst = self->inPlanar_.channel(c);
            if (in) for (int i = 0; i < n; ++i) dst[i] = in[(done + i) * inCh + c];
            else std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
        }
        self->engine_->process(self->inPlanar_.channels(), inCh, self->outPlanar_.channels(), outCh, n);
        for (int c = 0; c < outCh; ++c) {
            const float* src = self->outPlanar_.channel(c);
            for (int i = 0; i < n; ++i) out[(done + i) * outCh + c] = src[i];
        }
        done += static_cast<unsigned int>(n);
    }
}

bool DeviceManager::open(const AudioDeviceConfig& cfg, AudioEngine& engine, std::string* error) {
    close();
    if (!impl_->contextOk && !initialise(cfg.backend, error)) return false;

    auto findId = [](const std::vector<ma_device_info>& list, const std::string& name) -> const ma_device_id* {
        if (name.empty()) return nullptr;
        for (auto& d : list)
            if (name == d.name) return &d.id;
        return nullptr;
    };

    openNote_.clear();
    exclusiveActive_ = false;
    auto tryInit = [&](bool withInput, bool exclusive) -> ma_result {
    ma_device_config dc = ma_device_config_init(withInput ? ma_device_type_duplex : ma_device_type_playback);
    dc.sampleRate = static_cast<ma_uint32>(cfg.sampleRate);
    dc.periodSizeInFrames = static_cast<ma_uint32>(cfg.bufferSize);
    dc.periods = 2;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.playback.format = ma_format_f32;
    dc.playback.channels = static_cast<ma_uint32>(cfg.outputChannels);
    dc.playback.pDeviceID = findId(impl_->playback, cfg.outputDevice);
    dc.capture.format = ma_format_f32;
    dc.capture.channels = static_cast<ma_uint32>(cfg.inputChannels);
    dc.capture.pDeviceID = findId(impl_->capture, cfg.inputDevice);
    dc.noPreSilencedOutputBuffer = MA_TRUE;
    dc.noClip = MA_TRUE;
    dc.dataCallback = [](ma_device* d, void* o, const void* i, ma_uint32 n) { DeviceManager::dataCallback(d, o, i, n); };
    dc.notificationCallback = [](const ma_device_notification* n) { DeviceManager::notificationCallback(n); };
    dc.pUserData = this;
    dc.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    dc.capture.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    return ma_device_init(&impl_->context, &dc, &impl_->device);
    };

    bool withInput = cfg.enableInput;
    bool exclusive = cfg.exclusive;
    ma_result r = tryInit(withInput, exclusive);
    if (r != MA_SUCCESS && exclusive) { // exclusive refused (format not supported / device in use)
        log::warn("device", "exclusive mode failed ({}), using shared mode", ma_result_description(r));
        openNote_ = std::string("exclusive mode not possible (") + ma_result_description(r) + ") - using shared mode";
        exclusive = false;
        r = tryInit(withInput, false);
    }
    if (r != MA_SUCCESS && withInput) { // no/blocked input: keep playback working
        log::warn("device", "duplex open failed ({}), opening playback only", ma_result_description(r));
        openNote_ += std::string(openNote_.empty() ? "" : "; ") + "audio input could not be opened (" + ma_result_description(r) +
                     ") - playback only. Check the microphone / Windows privacy settings";
        withInput = false;
        r = tryInit(false, exclusive);
    }
    if (r != MA_SUCCESS) {
        if (error) *error = std::string("device init failed: ") + ma_result_description(r);
        log::error("device", "device init failed: {}", ma_result_description(r));
        return false;
    }
    impl_->deviceOk = true;
    cfg_ = cfg;
    lost_ = false;
    lastProgress_ = -1;
    actualRate_ = impl_->device.sampleRate;
    actualBuffer_ = static_cast<int>(impl_->device.playback.internalPeriodSizeInFrames);
    if (actualBuffer_ <= 0) actualBuffer_ = cfg.bufferSize;
    outChannels_ = static_cast<int>(impl_->device.playback.channels);
    inChannels_ = withInput ? static_cast<int>(impl_->device.capture.channels) : 0;
    exclusiveActive_ = exclusive;

    const int maxBlock = std::max(cfg.bufferSize, actualBuffer_);
    engine_ = &engine;
    engine.prepare(actualRate_, maxBlock);
    inPlanar_.setSize(std::max(1, inChannels_), maxBlock * 4);
    outPlanar_.setSize(std::max(1, outChannels_), maxBlock * 4);
    log::info("device", "opened: {} Hz, buffer {} (requested {}), in {} ch, out {} ch", actualRate_, actualBuffer_,
              cfg.bufferSize, inChannels_, outChannels_);
    return true;
}

bool DeviceManager::start(std::string* error) {
    if (!impl_->deviceOk) {
        if (error) *error = "device not open";
        return false;
    }
    if (engine_) engine_->setDeviceRunning(true);
    stopping_ = false;
    const ma_result r = ma_device_start(&impl_->device);
    if (r != MA_SUCCESS) {
        if (engine_) engine_->setDeviceRunning(false);
        if (error) *error = std::string("device start failed: ") + ma_result_description(r);
        return false;
    }
    return true;
}

void DeviceManager::stop() {
    stopping_ = true; // a stop we asked for is not a device loss
    if (impl_->deviceOk && ma_device_is_started(&impl_->device)) ma_device_stop(&impl_->device);
    if (engine_) engine_->setDeviceRunning(false);
}

void DeviceManager::close() {
    stop();
    if (impl_->deviceOk) {
        ma_device_uninit(&impl_->device);
        impl_->deviceOk = false;
    }
    engine_ = nullptr;
}

bool DeviceManager::isOpen() const { return impl_->deviceOk; }
bool DeviceManager::isRunning() const { return impl_->deviceOk && ma_device_is_started(&impl_->device); }

void DeviceManager::notificationCallback(const void* notification) {
    auto* n = static_cast<const ma_device_notification*>(notification);
    auto* self = static_cast<DeviceManager*>(n->pDevice->pUserData);
    if (!self) return;
    switch (n->type) {
    case ma_device_notification_type_stopped:
        if (!self->stopping_.load()) self->markLost("the audio device stopped unexpectedly (unplugged, disabled or taken by another application)");
        break;
    case ma_device_notification_type_rerouted: log::info("device", "audio device rerouted by the system"); break;
    default: break;
    }
}

void DeviceManager::markLost(const char* reason) {
    const char* expected = nullptr;
    lostReason_.compare_exchange_strong(expected, reason);
    lost_.store(true);
}

std::string DeviceManager::lostReason() const {
    const char* r = lostReason_.load();
    return r ? r : "";
}

void DeviceManager::simulateDeviceLoss() {
    if (impl_->deviceOk && ma_device_is_started(&impl_->device)) {
        stopping_ = false;
        ma_device_stop(&impl_->device); // fires the same "stopped" notification as a real loss
    }
    markLost("simulated device loss (fault injection)");
}

DeviceManager::Health DeviceManager::poll(double now) {
    if (!lostHandled_) {
        if (!lost_.load() && isRunning()) { // callback stall watchdog
            const uint64_t c = callbacks_.load();
            if (c != lastCallbacks_ || lastProgress_ < 0) {
                lastCallbacks_ = c;
                lastProgress_ = now;
            } else if (now - lastProgress_ > kStallSeconds) {
                markLost("the audio device stopped delivering audio (driver stalled)");
            }
        }
        if (!lost_.load()) return Health::Ok;
        log::error("device", "audio device lost: {}", lostReason());
        lostEngine_ = engine_;
        close();
        lostHandled_ = true;
        reconnectAttempts_ = 0;
        nextRetry_ = now + 1.0;
        return Health::Lost;
    }
    if (now < nextRetry_ || !lostEngine_) return Health::StillLost;
    ++reconnectAttempts_;
    nextRetry_ = now + std::min(5.0, 1.0 * reconnectAttempts_);
    if (simulateAbsent_) return Health::StillLost;
    AudioDeviceConfig cfg = cfg_;
    std::string err;
    // the device list may have changed: re-enumerate, then try the same device,
    // and from the second attempt on the system default.
    if (!initialise(cfg.backend, &err)) return Health::StillLost;
    bool ok = open(cfg, *lostEngine_, &err) && start(&err);
    if (!ok && reconnectAttempts_ >= 2 && (!cfg.outputDevice.empty() || !cfg.inputDevice.empty())) {
        close();
        cfg.outputDevice.clear();
        cfg.inputDevice.clear();
        ok = open(cfg, *lostEngine_, &err) && start(&err);
    }
    if (!ok) {
        close();
        log::warn("device", "reconnect attempt {} failed: {}", reconnectAttempts_, err);
        return Health::StillLost;
    }
    log::info("device", "audio device reconnected after {} attempt(s)", reconnectAttempts_);
    lostHandled_ = false;
    lostReason_.store(nullptr);
    lostEngine_ = nullptr;
    return Health::Reconnected;
}

int DeviceManager::latencySamples() const {
    if (!impl_->deviceOk) return 0;
    const auto& d = impl_->device;
    const int out = static_cast<int>(d.playback.internalPeriodSizeInFrames * d.playback.internalPeriods);
    const int in = inChannels_ > 0 ? static_cast<int>(d.capture.internalPeriodSizeInFrames) : 0;
    return out + in;
}

} // namespace roy
