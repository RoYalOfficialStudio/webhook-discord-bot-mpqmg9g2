#include "pluginhost/HostService.h"
#include "core/Base64.h"
#include "pluginhost/ClapHost.h"
#include "pluginhost/Vst3Host.h"
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

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct LineQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool eof = false;
};

json scanClap(const std::string& modulePath) {
    std::string err;
    auto m = clap::Module::load(modulePath, &err);
    if (!m) return {{"ok", false}, {"path", modulePath}, {"error", err}};
    json plugins = m->describeAll();
    for (auto& p : plugins) {
        std::string ierr;
        auto inst = clap::Instance::create(*m, p["id"].get<std::string>(), &ierr);
        p["instantiates"] = inst != nullptr;
        if (inst) {
            p["paramCount"] = inst->params().size();
            p["inputChannels"] = inst->inputChannels();
            p["outputChannels"] = inst->outputChannels();
            p["hasEditor"] = inst->editorSupported();
        } else {
            p["error"] = ierr;
        }
    }
    return {{"ok", true}, {"path", modulePath}, {"format", "clap"}, {"plugins", plugins}};
}

} // namespace

int runScan(const std::string& modulePath) {
    std::string p = lower(modulePath);
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const bool vst3 = p.size() > 5 && p.substr(p.size() - 5) == ".vst3";
    reply(vst3 ? vst3::scanModule(modulePath) : scanClap(modulePath));
    return 0;
}

int runHost(const std::string& format, const std::string& modulePath, const std::string& pluginId, const std::string& shmName) {
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
    std::unique_ptr<clap::Module> clapModule;
    std::unique_ptr<HostedPlugin> inst;
    if (format == "clap") {
        clapModule = clap::Module::load(modulePath, &err);
        if (clapModule) inst = clap::Instance::create(*clapModule, pluginId, &err);
    } else if (format == "vst3") {
        inst = vst3::createInstance(modulePath, pluginId, &err);
    } else {
        err = "unknown plugin format " + format;
    }
    if (!inst) {
        reply({{"ok", false}, {"error", err}});
        return 1;
    }
    reply({{"ok", true}, {"info", inst->info()}});

    std::mutex procMutex; // host-side only: serialises activation/state against processing
    std::atomic<bool> quit{false};

    std::thread audio([&] {
        while (!quit.load()) {
            if (!request.wait(100000)) continue;
            if (block->command == pipc::CmdQuit) break;
            const uint32_t seq = block->reqSeq.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lk(procMutex);
                block->numOutParams = 0;
                block->processStatus = inst->process(*block);
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
            q.cv.wait_for(lk, std::chrono::milliseconds(10), [&] { return !q.lines.empty() || q.eof; });
            if (q.lines.empty()) {
                if (q.eof) break; // parent went away
                lk.unlock();
                inst->idle(); // GUI events, timers, main-thread callbacks
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
            bool ok;
            {
                std::lock_guard<std::mutex> lk(procMutex);
                ok = inst->saveState(data);
            }
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
            reply({{"ok", ok}, {"values", inst->paramValues()}});
        } else if (c == "params") {
            reply({{"ok", true}, {"values", inst->paramValues()}});
        } else if (c == "latency") {
            reply({{"ok", true}, {"latency", inst->latency()}});
        } else if (c == "editor.open") {
            std::string e;
            const bool ok = inst->openEditor(cmd.value("alwaysOnTop", false), &e);
            reply({{"ok", ok}, {"error", e}, {"editor", inst->editorState()}});
        } else if (c == "editor.close") {
            inst->closeEditor();
            reply({{"ok", true}, {"editor", inst->editorState()}});
        } else if (c == "editor.state") {
            for (int i = 0; i < 5; ++i) inst->idle(); // let pending window events settle
            reply({{"ok", true}, {"editor", inst->editorState()}});
        } else {
            reply({{"ok", false}, {"error", "unknown command " + c}});
        }
    }
    quit = true;
    request.post();
    audio.join();
    {
        std::lock_guard<std::mutex> lk(procMutex);
        inst->shutdown(); // deactivate, terminate, unload
        inst.reset();
    }
    clapModule.reset();
    std::fflush(protocolOut());
    std::_Exit(0); // do not wait for the detached stdin reader
}

} // namespace roy::pluginhost
