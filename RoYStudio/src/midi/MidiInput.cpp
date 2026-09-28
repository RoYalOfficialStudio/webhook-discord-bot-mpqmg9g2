#include "midi/MidiInput.h"
#include "core/Log.h"
#include "midi/Scale.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace roy::midi {

namespace fs = std::filesystem;

std::string describeMessage(uint8_t status, uint8_t d1, uint8_t d2) {
    const int ch = (status & 0x0F) + 1;
    switch (status & 0xF0) {
    case 0x90:
        if (d2 > 0) return std::format("Note On {} vel {} (ch {})", noteName(d1), d2, ch);
        [[fallthrough]];
    case 0x80: return std::format("Note Off {} (ch {})", noteName(d1), ch);
    case 0xB0: return d1 == 64 ? std::format("Sustain {} (ch {})", d2 >= 64 ? "down" : "up", ch) : std::format("CC {} = {} (ch {})", d1, d2, ch);
    case 0xE0: return std::format("Pitch Bend {} (ch {})", ((d2 << 7) | d1) - 8192, ch);
    case 0xC0: return std::format("Program {} (ch {})", d1, ch);
    case 0xD0: return std::format("Aftertouch {} (ch {})", d1, ch);
    case 0xA0: return std::format("Poly Aftertouch {} {} (ch {})", noteName(d1), d2, ch);
    default: return "-";
    }
}

struct MidiInputManager::Input {
    std::string id;
    int port = 0;
    MidiInputManager* owner = nullptr;
    MidiParser parser;
#ifdef _WIN32
    HMIDIIN handle = nullptr;
#else
    int fd = -1;
    std::thread reader;
    std::atomic<bool> stop{false};
#endif
};

MidiInputManager::MidiInputManager(AudioEngine& engine) : engine_(engine) {
    virtual_ = std::make_unique<Input>();
    virtual_->id = "virtual";
}

MidiInputManager::~MidiInputManager() { closeAll(); }

void MidiInputManager::deliver(Input& in, const uint8_t* data, size_t n) {
    std::lock_guard l(mutex_); // serialises producers; the audio thread never takes it
    in.parser.feed(data, n, [&](uint8_t s, uint8_t d1, uint8_t d2) {
        engine_.pushLiveMidi(LiveMidiMessage{s, d1, d2, static_cast<uint8_t>(in.port)});
        messages_.fetch_add(1, std::memory_order_relaxed);
        last_.store(static_cast<uint32_t>(s) | static_cast<uint32_t>(d1) << 8 | static_cast<uint32_t>(d2) << 16, std::memory_order_relaxed);
    });
}

void MidiInputManager::inject(const uint8_t* data, size_t n, int port) {
    virtual_->port = port;
    deliver(*virtual_, data, n);
}

std::string MidiInputManager::lastMessageText() const {
    const uint32_t v = last_.load(std::memory_order_relaxed);
    if (!messages_.load(std::memory_order_relaxed)) return "no MIDI received yet";
    return describeMessage(static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16));
}

std::vector<std::string> MidiInputManager::openDevices() const {
    std::lock_guard l(mutex_);
    std::vector<std::string> v;
    for (auto& i : inputs_) v.push_back(i->id);
    return v;
}

bool MidiInputManager::isOpen(const std::string& id) const {
    std::lock_guard l(mutex_);
    return std::any_of(inputs_.begin(), inputs_.end(), [&](auto& i) { return i->id == id; });
}

#ifdef _WIN32
// ---------------------------------------------------------------- Windows: WinMM
namespace {
int messageLength(uint8_t status) {
    if (status < 0x80 || status >= 0xF8) return 0;
    switch (status & 0xF0) {
    case 0xC0:
    case 0xD0: return 2;
    case 0xF0: return 0; // system common/SysEx: not used
    default: return 3;
    }
}
} // namespace

struct MidiInputAccess {
    static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* in = reinterpret_cast<MidiInputManager::Input*>(instance);
        const uint8_t bytes[3] = {static_cast<uint8_t>(p1), static_cast<uint8_t>(p1 >> 8), static_cast<uint8_t>(p1 >> 16)};
        const int len = messageLength(bytes[0]); // WinMM packs one short message; feed only its bytes
        if (len > 0 && in && in->owner) in->owner->deliver(*in, bytes, static_cast<size_t>(len));
    }
};

std::vector<MidiInputDevice> MidiInputManager::devices() const {
    std::vector<MidiInputDevice> v;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name) - 1, nullptr, nullptr);
        v.push_back({std::format("winmm:{}", i), name});
    }
    return v;
}

bool MidiInputManager::open(const std::string& id, std::string* error) {
    if (isOpen(id)) return true;
    if (id.rfind("winmm:", 0) != 0) {
        if (error) *error = "unknown MIDI device id " + id;
        return false;
    }
    const UINT index = static_cast<UINT>(std::atoi(id.c_str() + 6));
    auto in = std::make_unique<Input>();
    in->id = id;
    in->port = static_cast<int>(index) + 1;
    in->owner = this;
    const MMRESULT r = midiInOpen(&in->handle, index, reinterpret_cast<DWORD_PTR>(&MidiInputAccess::proc),
                                  reinterpret_cast<DWORD_PTR>(in.get()), CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        if (error) *error = std::format("cannot open MIDI input {} (WinMM error {}) - is it used by another program?", id, r);
        return false;
    }
    midiInStart(in->handle);
    std::lock_guard l(mutex_);
    inputs_.push_back(std::move(in));
    log::info("midi", "opened MIDI input {}", id);
    return true;
}

void MidiInputManager::close(const std::string& id) {
    std::unique_ptr<Input> victim;
    {
        std::lock_guard l(mutex_);
        auto it = std::find_if(inputs_.begin(), inputs_.end(), [&](auto& i) { return i->id == id; });
        if (it == inputs_.end()) return;
        victim = std::move(*it);
        inputs_.erase(it);
    }
    midiInStop(victim->handle);
    midiInReset(victim->handle);
    midiInClose(victim->handle); // waits for running callbacks
}
#else
// ---------------------------------------------------------------- Linux: ALSA raw MIDI device files
std::vector<MidiInputDevice> MidiInputManager::devices() const {
    std::vector<MidiInputDevice> v;
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/dev/snd", ec)) {
        const std::string f = e.path().filename().string();
        if (f.rfind("midiC", 0) != 0) continue;
        std::string name = f;
        const auto d = f.find('D');
        if (d != std::string::npos) { // card name from /proc/asound/card<N>/id
            std::ifstream idf("/proc/asound/card" + f.substr(5, d - 5) + "/id");
            std::string card;
            if (std::getline(idf, card) && !card.empty()) name = card + " (" + f + ")";
        }
        v.push_back({e.path().string(), name});
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.id < b.id; });
    return v;
}

bool MidiInputManager::open(const std::string& id, std::string* error) {
    if (isOpen(id)) return true;
    const int fd = ::open(id.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (error) *error = std::format("cannot open MIDI input {}: {}", id, std::strerror(errno));
        return false;
    }
    auto in = std::make_unique<Input>();
    in->id = id;
    in->fd = fd;
    {
        std::lock_guard l(mutex_);
        in->port = static_cast<int>(inputs_.size()) + 1;
    }
    Input* raw = in.get();
    in->reader = std::thread([this, raw] {
        uint8_t buf[256];
        while (!raw->stop.load()) {
            pollfd p{raw->fd, POLLIN, 0};
            const int r = ::poll(&p, 1, 50);
            if (r <= 0) continue;
            const ssize_t n = ::read(raw->fd, buf, sizeof(buf));
            if (n > 0) deliver(*raw, buf, static_cast<size_t>(n));
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                if (p.revents & (POLLHUP | POLLERR)) std::this_thread::sleep_for(std::chrono::milliseconds(50)); // device gone / writer closed
            }
        }
    });
    std::lock_guard l(mutex_);
    inputs_.push_back(std::move(in));
    log::info("midi", "opened MIDI input {}", id);
    return true;
}

void MidiInputManager::close(const std::string& id) {
    std::unique_ptr<Input> victim;
    {
        std::lock_guard l(mutex_);
        auto it = std::find_if(inputs_.begin(), inputs_.end(), [&](auto& i) { return i->id == id; });
        if (it == inputs_.end()) return;
        victim = std::move(*it);
        inputs_.erase(it);
    }
    victim->stop = true;
    if (victim->reader.joinable()) victim->reader.join();
    ::close(victim->fd);
}
#endif

void MidiInputManager::closeAll() {
    for (auto& id : openDevices()) close(id);
}

} // namespace roy::midi
