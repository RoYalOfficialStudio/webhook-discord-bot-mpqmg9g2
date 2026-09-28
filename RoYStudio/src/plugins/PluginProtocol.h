#pragma once
// Shared-memory block exchanged between RoY Studio (DAW side) and the sandboxed
// plugin host process (RoYPluginHost). One region per plugin instance.
//
//   DAW audio thread                           host audio thread
//   ----------------                           -----------------
//   write in[], events, param changes
//   seq = ++reqSeq; request.post()  ------->   request.wait()
//                                              plugin->process(...)
//   response.wait(timeout)          <-------   doneSeq = seq; response.post()
//   read out[]
//
// Control traffic (init, state save/load, activation) goes as JSON lines over the
// host's stdin/stdout. Nothing in this struct owns heap memory.
#include <atomic>
#include <cstdint>

namespace roy::pluginipc {

constexpr uint32_t kMagic = 0x524F5950; // 'ROYP'
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxFrames = 4096;
constexpr uint32_t kMaxEvents = 1024;
constexpr uint32_t kMaxParamChanges = 512;

enum EventType : uint8_t { EvNoteOn = 0, EvNoteOff, EvAllNotesOff, EvPitchBend, EvController };

struct Event {
    uint32_t offset;
    uint8_t type;
    uint8_t channel;
    int16_t note;
    float velocity;
    float value;
    int16_t controller;
    int16_t pad;
};

struct ParamChange {
    uint32_t offset;
    uint32_t paramId; // CLAP param id
    double value;
};

enum Command : uint32_t { CmdProcess = 1, CmdQuit = 2 };

// Storage large enough for a POSIX sem_t placed in shared memory.
struct alignas(64) SignalStorage {
    unsigned char bytes[64];
};

struct alignas(64) Block {
    uint32_t magic;
    uint32_t version;
    SignalStorage request;
    SignalStorage response;
    std::atomic<uint32_t> reqSeq;
    std::atomic<uint32_t> doneSeq;
    uint32_t command;
    uint32_t frames;
    uint32_t numEvents;
    uint32_t numParamChanges;
    int32_t processStatus; // CLAP process status or -1 on host error
    uint32_t transportFlags; // bit0 playing
    double tempo;
    int64_t steadyTime;
    int64_t songPosSamples;   // timeline position of the first frame
    uint32_t numOutParams;    // plugin -> RoY: parameter changes (automation from the plugin GUI / plugin itself)
    float in[2][kMaxFrames];
    float out[2][kMaxFrames];
    Event events[kMaxEvents];
    ParamChange params[kMaxParamChanges];
    ParamChange outParams[kMaxParamChanges];
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics required in shared memory");

} // namespace roy::pluginipc
