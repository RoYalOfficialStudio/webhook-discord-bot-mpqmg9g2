#pragma once
// LIVE MIDI INPUT (MIDI keyboards, pad controllers).
// Backends: Windows = WinMM (system API, no extra dependency); Linux = ALSA raw MIDI devices
// (/dev/snd/midiC*D*, read directly, no libasound needed). Every device gets its own reader;
// all of them feed ONE parser path into AudioEngine::pushLiveMidi (serialised here, so the
// engine queue keeps a single producer and the audio thread stays lock-free).
#include "audio/AudioEngine.h"
#include "midi/MidiLearn.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace roy::midi {

struct MidiInputDevice {
    std::string id;   // "winmm:<device name>" (stable across re-plugging), "/dev/snd/midiC1D0" or any byte-stream path
    std::string name; // human readable
};

// MIDI byte-stream parser: running status, real-time bytes (clock, active sensing) inside
// messages, SysEx skipped, note-on with velocity 0 kept as is (the engine treats it as off).
class MidiParser {
public:
    template <class Emit>
    void feed(const uint8_t* d, size_t n, Emit&& emit) {
        for (size_t i = 0; i < n; ++i) {
            const uint8_t b = d[i];
            if (b >= 0xF8) continue;             // real-time: ignore, keep the running message
            if (b == 0xF0) { sysex_ = true; continue; }
            if (b == 0xF7) { sysex_ = false; status_ = 0; continue; }
            if (b & 0x80) {
                sysex_ = false;
                status_ = b >= 0xF0 ? 0 : b;     // system common cancels running status
                have_ = 0;
                continue;
            }
            if (sysex_ || status_ == 0) continue;
            data_[have_++] = b;
            const int type = status_ & 0xF0;
            const int need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (have_ == need) {
                emit(status_, data_[0], need == 2 ? data_[1] : uint8_t(0));
                have_ = 0;                       // running status stays active
            }
        }
    }

private:
    uint8_t status_ = 0;
    uint8_t data_[2] = {};
    int have_ = 0;
    bool sysex_ = false;
};

class MidiInputManager {
public:
    explicit MidiInputManager(AudioEngine& engine);
    ~MidiInputManager();
    MidiInputManager(const MidiInputManager&) = delete;
    MidiInputManager& operator=(const MidiInputManager&) = delete;

    std::vector<MidiInputDevice> devices() const;
    // Opens a device by id (see devices()) or, on Linux, any readable byte-stream path.
    bool open(const std::string& id, std::string* error = nullptr);
    void close(const std::string& id);
    void closeAll();
    std::vector<std::string> openDevices() const;
    bool isOpen(const std::string& id) const;

    // HOT-PLUG: opens inputs that appeared since the last call (except `skip`, e.g. inputs the
    // user switched off) and closes inputs whose device is gone. Cheap; call every ~2 s.
    struct RescanResult {
        std::vector<MidiInputDevice> added;
        std::vector<std::string> removed; // ids
        std::vector<std::string> failed;  // "name: error"
    };
    RescanResult rescan(const std::vector<std::string>& skip = {});
#ifndef _WIN32
    // Where raw MIDI device files are listed (default /dev/snd). Tests point it at FIFOs.
    void setDeviceDirectory(const std::string& dir) { deviceDir_ = dir; }
#endif

    // Feeds bytes as if they came from input `port` (on-screen keyboard, tests).
    void inject(const uint8_t* data, size_t n, int port = 0);

    // Controller moves for the message thread (MIDI learn / mapped controls); bounded, the
    // oldest are dropped if nobody drains.
    std::vector<ControlChange> drainControlChanges();
    // Controllers that drive mapped parameters are consumed here (not sent to the instrument).
    // `anyChannel` entries block the CC on all 16 channels. While learning, every learnable
    // CC is consumed so a knob turned to learn does not also change the synth.
    void setConsumedControls(const std::vector<std::pair<int, int>>& channelAndCc);
    void setLearning(bool on) { learning_.store(on, std::memory_order_relaxed); }
    bool isConsumed(int channel, int cc) const;

    uint64_t messageCount() const { return messages_.load(std::memory_order_relaxed); }
    // "Note On C3 vel 100" - last message, for the activity indicator.
    std::string lastMessageText() const;

private:
    struct Input;
    void deliver(Input& in, const uint8_t* data, size_t n);
    AudioEngine& engine_;
    mutable std::mutex mutex_;     // device list + parser/producer serialisation (never the audio thread)
    std::vector<std::unique_ptr<Input>> inputs_;
    std::unique_ptr<Input> virtual_;
    std::atomic<uint64_t> messages_{0};
    std::vector<ControlChange> ccQueue_;          // guarded by mutex_
    std::array<std::atomic<uint64_t>, 32> consumed_{}; // 16 channels x 128 CC bits
    std::atomic<bool> learning_{false};
#ifndef _WIN32
    std::string deviceDir_ = "/dev/snd";
#endif
    std::atomic<uint32_t> last_{0};
    friend struct MidiInputAccess;
};

std::string describeMessage(uint8_t status, uint8_t d1, uint8_t d2);

} // namespace roy::midi
