#include "plugins/HostService.h"
#include "core/Base64.h"
#include "plugins/ClapHost.h"
#include "plugins/PluginProtocol.h"
#include "plugins/SharedMemory.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace roy::pluginhost {

using json = nlohmann::json;
namespace pipc = roy::pluginipc;

namespace {

// Plugins sometimes print to stdout. The protocol gets a private duplicate of
// stdout and fd 1 is redirected to stderr, so plugin output cannot corrupt it.
FILE* protocolOut() {
    static FILE* out = [] {
#ifdef _WIN32
        const int fd = _dup(_fileno(stdout));
        _dup2(_fileno(stderr), _fileno(stdout));
        _setmode(fd, _O_BINARY);
        return _fdopen(fd, "wb");
#else
        std::fflush(stdout);
        const int fd = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        return fdopen(fd, "w");
#endif
    }();
    return out;
}

void reply(const json& j) {
    FILE* f = protocolOut();
    const std::string s = j.dump() + "\n";
    std::fwrite(s.data(), 1, s.size(), f);
    std::fflush(f);
}

// Fixed-capacity CLAP input event list built on the host audio thread (no allocation).
union AnyEvent {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_param_value_t param;
    clap_event_midi_t midi;
};

struct EventList {
    clap_input_events_t list{};
    AnyEvent events[pipc::kMaxEvents + pipc::kMaxParamChanges];
    uint32_t count = 0;
    EventList() {
        list.ctx = this;
        list.size = [](const clap_input_events_t* l) { return static_cast<const EventList*>(l->ctx)->count; };
        list.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
            auto* self = static_cast<const EventList*>(l->ctx);
            return i < self->count ? &self->events[i].header : nullptr;
        };
    }
    void addParam(const pipc::ParamChange& p) {
        AnyEvent& e = events[count++];
        e.param = {};
        e.param.header = {sizeof(clap_event_param_value_t), p.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param.param_id = p.paramId;
        e.param.note_id = -1;
        e.param.port_index = -1;
        e.param.channel = -1;
        e.param.key = -1;
        e.param.value = p.value;
    }
    void addNote(const pipc::Event& n) {
        AnyEvent& e = events[count++];
        switch (n.type) {
        case pipc::EvNoteOn:
        case pipc::EvNoteOff:
            e.note = {};
            e.note.header = {sizeof(clap_event_note_t), n.offset, CLAP_CORE_EVENT_SPACE_ID,
                             static_cast<uint16_t>(n.type == pipc::EvNoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note.note_id = -1;
            e.note.port_index = 0;
            e.note.channel = n.channel;
            e.note.key = n.note;
            e.note.velocity = n.velocity;
            break;
        case pipc::EvAllNotesOff:
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xB0 | (n.channel & 15));
            e.midi.data[1] = 123;
            e.midi.data[2] = 0;
            break;
        case pipc::EvPitchBend: {
            const int v = std::clamp(static_cast<int>(std::lround((n.value + 1.0f) * 8191.5f)), 0, 16383);
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xE0 | (n.channel & 15));
            e.midi.data[1] = static_cast<uint8_t>(v & 127);
            e.midi.data[2] = static_cast<uint8_t>(v >> 7);
            break;
        }
        default: // controller
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xB0 | (n.channel & 15));
            e.midi.data[1] = static_cast<uint8_t>(n.controller & 127);
            e.midi.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(n.value * 127.0f + 0.5f), 0, 127));
            break;
        }
    }
    // Merge two offset-sorted inputs into one time-ordered list (CLAP requires ordering).
    void build(const pipc::Block& b) {
        count = 0;
        const uint32_t np = std::min(b.numParamChanges, pipc::kMaxParamChanges);
        const uint32_t ne = std::min(b.numEvents, pipc::kMaxEvents);
        uint32_t i = 0, k = 0;
        while (i < np || k < ne) {
            const bool takeParam = k >= ne || (i < np && b.params[i].offset <= b.events[k].offset);
            if (takeParam) addParam(b.params[i++]);
            else addNote(b.events[k++]);
        }
    }
};

struct LineQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool eof = false;
};

} // namespace

int runScan(const std::string& modulePath) {
    std::string err;
    auto m = clap::Module::load(modulePath, &err);
    if (!m) {
        reply({{"ok", false}, {"path", modulePath}, {"error", err}});
        return 0;
    }
    json plugins = m->describeAll();
    // Instantiate each plugin once to verify it really loads and to count parameters.
    for (auto& p : plugins) {
        std::string ierr;
        auto inst = clap::Instance::create(*m, p["id"].get<std::string>(), &ierr);
        p["instantiates"] = inst != nullptr;
        if (inst) {
            p["paramCount"] = inst->params().size();
            p["inputChannels"] = inst->inputChannels();
            p["outputChannels"] = inst->outputChannels();
        } else {
            p["error"] = ierr;
        }
    }
    reply({{"ok", true}, {"path", modulePath}, {"plugins", plugins}});
    return 0;
}

int runHost(const std::string& modulePath, const std::string& pluginId, const std::string& shmName) {
    ipc::SharedRegion region;
    std::string err;
    if (!region.open(shmName, sizeof(pipc::Block), &err)) {
        reply({{"ok", false}, {"error", err}});
        return 1;
    }
    auto* block = static_cast<pipc::Block*>(region.data());
    if (block->magic != pipc::kMagic || block->version != pipc::kVersion) {
        reply({{"ok", false}, {"error", "shared memory protocol mismatch"}});
        return 1;
    }
    ipc::Signal request, response;
    if (!request.open(shmName + "_req", &block->request) || !response.open(shmName + "_rsp", &block->response)) {
        reply({{"ok", false}, {"error", "cannot open signals"}});
        return 1;
    }
    auto module = clap::Module::load(modulePath, &err);
    if (!module) {
        reply({{"ok", false}, {"error", err}});
        return 1;
    }
    auto inst = clap::Instance::create(*module, pluginId, &err);
    if (!inst) {
        reply({{"ok", false}, {"error", err}});
        return 1;
    }
    reply({{"ok", true}, {"info", inst->info()}});

    std::mutex procMutex; // host-side only: serialises activation against processing
    std::atomic<bool> quit{false};
    auto events = std::make_unique<EventList>();

    std::thread audio([&] {
        clap_event_transport_t transport{};
        transport.header = {sizeof(clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        while (!quit.load()) {
            if (!request.wait(100000)) continue;
            if (block->command == pipc::CmdQuit) break;
            const uint32_t seq = block->reqSeq.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lk(procMutex);
                const uint32_t frames = std::min(block->frames, pipc::kMaxFrames);
                events->build(*block);
                transport.flags = static_cast<uint32_t>(CLAP_TRANSPORT_HAS_TEMPO) | ((block->transportFlags & 1u) ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
                transport.tempo = block->tempo;
                float* in[2] = {block->in[0], block->in[1]};
                float* out[2] = {block->out[0], block->out[1]};
                // Mono-output plugins: mirror channel 0.
                block->processStatus = inst->process(inst->inputChannels() > 0 ? in : nullptr, out, frames, &events->list, &transport);
                if (inst->outputChannels() == 1) std::memcpy(block->out[1], block->out[0], sizeof(float) * frames);
            }
            block->doneSeq.store(seq, std::memory_order_release);
            response.post();
        }
    });

    LineQueue q;
    std::thread reader([&q] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(q.m);
            q.lines.push_back(line);
            q.cv.notify_one();
        }
        std::lock_guard<std::mutex> lk(q.m);
        q.eof = true;
        q.cv.notify_one();
    });
    reader.detach(); // blocked in getline; the process exits after the loop

    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(q.m);
            q.cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return !q.lines.empty() || q.eof; });
            if (q.lines.empty()) {
                if (q.eof) break; // parent went away
                inst->idle();
                continue;
            }
            line = std::move(q.lines.front());
            q.lines.pop_front();
        }
        inst->idle();
        json cmd = json::parse(line, nullptr, false);
        if (cmd.is_discarded() || !cmd.is_object()) {
            reply({{"ok", false}, {"error", "bad command"}});
            continue;
        }
        const std::string c = cmd.value("cmd", "");
        if (c == "quit") {
            reply({{"ok", true}});
            break;
        } else if (c == "ping") {
            reply({{"ok", true}});
        } else if (c == "activate") {
            std::lock_guard<std::mutex> lk(procMutex);
            const double sr = cmd.value("sampleRate", 48000.0);
            const uint32_t mb = std::min<uint32_t>(cmd.value("maxFrames", 512u), pipc::kMaxFrames);
            const bool ok = inst->activate(sr, mb);
            reply({{"ok", ok}, {"latency", inst->latency()}, {"error", ok ? "" : "activate failed"}});
        } else if (c == "reset") {
            std::lock_guard<std::mutex> lk(procMutex);
            inst->reset();
            reply({{"ok", true}});
        } else if (c == "state.save") {
            std::vector<uint8_t> data;
            const bool ok = inst->saveState(data);
            reply({{"ok", ok}, {"state", ok ? b64::encode(data.data(), data.size()) : ""}});
        } else if (c == "state.load") {
            std::vector<uint8_t> data;
            if (!b64::decode(cmd.value("state", ""), data)) {
                reply({{"ok", false}, {"error", "bad base64"}});
                continue;
            }
            bool ok;
            {
                std::lock_guard<std::mutex> lk(procMutex);
                ok = inst->loadState(data);
            }
            json values = json::object();
            for (auto& p : inst->params()) values[std::to_string(p.id)] = inst->paramValue(p.id);
            reply({{"ok", ok}, {"values", values}});
        } else if (c == "params") {
            json values = json::object();
            for (auto& p : inst->params()) values[std::to_string(p.id)] = inst->paramValue(p.id);
            reply({{"ok", true}, {"values", values}});
        } else {
            reply({{"ok", false}, {"error", "unknown command " + c}});
        }
    }
    quit = true;
    request.post();
    audio.join();
    {
        std::lock_guard<std::mutex> lk(procMutex);
        inst.reset();
    }
    module.reset();
    std::fflush(protocolOut());
    std::_Exit(0); // do not wait for the detached stdin reader
}

} // namespace roy::pluginhost
