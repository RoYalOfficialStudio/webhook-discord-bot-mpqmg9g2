#include "plugins/Sandbox.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Process.h"
#include "plugins/PluginProtocol.h"
#include <clap/clap.h>
#include "plugins/SharedMemory.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <thread>

namespace roy {

namespace pipc = roy::pluginipc;

namespace plugins {

namespace {
std::mutex g_mutex;
std::string g_hostExe;
std::string g_crashDir;
std::atomic<int> g_timeoutMs{250};
std::vector<CrashEvent> g_crashes;

void reportCrash(const std::string& typeId, const std::string& name, const std::string& reason) {
    CrashEvent e{typeId, name, reason, files::nowIso8601()};
    log::error("plugins", "PLUGIN CRASHED: {} ({}) - {}. Plugin deactivated (pass-through/silence), the project continues.", name,
               typeId, reason);
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_crashes.push_back(e);
        dir = g_crashDir;
    }
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        json j = {{"type", "PLUGIN CRASHED"}, {"plugin", name}, {"typeId", typeId}, {"reason", reason}, {"time", e.time},
                  {"host", hostName()}, {"recentLog", log::recentLines(40)}};
        files::atomicWrite(files::uniquePath(std::filesystem::path(dir) / std::format("plugin_crash_{}.json", files::nowCompact())),
                           j.dump(2));
    }
}
} // namespace

void setHostExecutable(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_hostExe = path;
}
std::string hostExecutable() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_hostExe.empty()) return g_hostExe;
#ifdef _WIN32
    return (std::filesystem::path(executableDirectory()) / "roy_plugin_host.exe").string();
#else
    return (std::filesystem::path(executableDirectory()) / "roy_plugin_host").string();
#endif
}
void setCrashReportDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_crashDir = dir;
}
void setProcessTimeoutMs(int ms) { g_timeoutMs.store(std::max(1, ms)); }
int processTimeoutMs() { return g_timeoutMs.load(); }

std::vector<CrashEvent> takeCrashEvents() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<CrashEvent> out;
    out.swap(g_crashes);
    return out;
}

std::string makeClapTypeId(const std::string& modulePath, const std::string& pluginId) { return "clap:" + modulePath + "|" + pluginId; }

std::string makeVst3TypeId(const std::string& modulePath, const std::string& classId) { return "vst3:" + modulePath + "|" + classId; }

bool parsePluginTypeId(const std::string& typeId, std::string& format, std::string& modulePath, std::string& pluginId) {
    if (typeId.rfind("clap:", 0) == 0) format = "clap";
    else if (typeId.rfind("vst3:", 0) == 0) format = "vst3";
    else return false;
    const auto bar = typeId.rfind('|');
    if (bar == std::string::npos || bar < 6) return false;
    modulePath = typeId.substr(5, bar - 5);
    pluginId = typeId.substr(bar + 1);
    return !modulePath.empty() && !pluginId.empty();
}

// ------------------------------------------------------------------ Sandbox
class Sandbox {
public:
    enum State { Running = 0, Crashed = 1, Hung = 2, Stopped = 3 };

    ~Sandbox() { shutdown(); }

    bool launch(const std::string& format, const std::string& module, const std::string& pluginId, const std::string& typeId, json& info,
                std::string* error) {
        typeId_ = typeId;
        name_ = pluginId;
        shmName_ = ipc::uniqueIpcName("plug");
        if (!region_.create(shmName_, sizeof(pipc::Block), error)) return false;
        block_ = new (region_.data()) pipc::Block();
        block_->magic = pipc::kMagic;
        block_->version = pipc::kVersion;
        if (!request_.create(shmName_ + "_req", &block_->request) || !response_.create(shmName_ + "_rsp", &block_->response)) {
            if (error) *error = "cannot create IPC signals";
            return false;
        }
        const std::string exe = hostExecutable();
        if (!child_.start(exe, {"--host", format, module, pluginId, shmName_}, error)) return false;
        std::string line;
        if (!child_.readLine(line, 15000)) {
            child_.wait(200);
            if (error)
                *error = child_.crashed() ? std::format("plugin host crashed while loading ({})", child_.terminationReason())
                                          : "plugin host did not answer (timeout)";
            child_.kill();
            return false;
        }
        json r = json::parse(line, nullptr, false);
        if (r.is_discarded() || !r.value("ok", false)) {
            if (error) *error = r.is_object() ? r.value("error", std::string("load failed")) : "bad reply from plugin host";
            child_.kill();
            return false;
        }
        info = r["info"];
        name_ = info.value("name", pluginId);
        watchdog_ = std::thread([this] { watch(); });
        return true;
    }

    // Message thread. Returns false (and marks the sandbox crashed) if the host died.
    bool call(const json& cmd, json& reply, int timeoutMs = 5000) {
        std::lock_guard<std::mutex> lk(ioMutex_);
        if (state_.load() != Running) return false;
        const std::string s = cmd.dump() + "\n";
        std::string line;
        if (!child_.writeAll(s.data(), s.size())) return false;
        if (!child_.readLine(line, timeoutMs)) {
            if (child_.isRunning()) {
                // Alive but not answering: treat like an audio hang (the watchdog terminates it).
                controlTimeout_ = std::format("no reply to '{}' within {} ms", cmd.value("cmd", ""), timeoutMs);
                int expected = Running;
                state_.compare_exchange_strong(expected, Hung);
            }
            return false;
        }
        reply = json::parse(line, nullptr, false);
        return reply.is_object() && reply.value("ok", false);
    }

    // Audio thread. true = output valid.
    bool process(uint32_t seq, int timeoutMicros) noexcept {
        if (state_.load(std::memory_order_acquire) != Running) return false;
        block_->command = pipc::CmdProcess;
        block_->reqSeq.store(seq, std::memory_order_release);
        request_.post();
        const bool signalled = response_.wait(timeoutMicros);
        if (state_.load(std::memory_order_acquire) != Running) return false;
        if (!signalled || block_->doneSeq.load(std::memory_order_acquire) != seq) {
            int expected = Running;
            state_.compare_exchange_strong(expected, Hung);
            return false;
        }
        return block_->processStatus != CLAP_PROCESS_ERROR;
    }

    pipc::Block* block() const { return block_; }
    int state() const { return state_.load(); }
    std::string problem() const {
        std::lock_guard<std::mutex> lk(reasonMutex_);
        return reason_;
    }
    int pid() const { return child_.pid(); }
    const std::string& name() const { return name_; }

    void killForTest() {
        std::lock_guard<std::mutex> lk(ioMutex_);
        child_.kill();
        // the watchdog notices on its next poll: kill() clears crashed(), so tag the reason here
        std::lock_guard<std::mutex> lk2(reasonMutex_);
        testKill_ = true;
    }

private:
    void setProblem(const std::string& r) {
        std::lock_guard<std::mutex> lk(reasonMutex_);
        reason_ = r;
    }

    void watch() {
        while (!stopWatch_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const int st = state_.load();
            if (st == Stopped) return;
            if (st == Hung) {
                {
                    std::lock_guard<std::mutex> lk(ioMutex_);
                    child_.kill();
                }
                const std::string r = controlTimeout_.empty()
                                          ? std::format("hung (no audio response within {} ms) - host process terminated", processTimeoutMs())
                                          : std::format("hung ({}) - host process terminated", controlTimeout_);
                response_.post();
                reportCrash(typeId_, name_, r); // logged + queued before problem() becomes visible
                setProblem(r);
                return;
            }
            std::unique_lock<std::mutex> lk(ioMutex_, std::try_to_lock);
            if (!lk.owns_lock()) continue;
            if (!child_.isRunning()) {
                int expected = Running;
                if (!state_.compare_exchange_strong(expected, Crashed)) {
                    if (expected == Hung) continue; // handled above on the next loop
                    return;
                }
                bool test;
                {
                    std::lock_guard<std::mutex> lk2(reasonMutex_);
                    test = testKill_;
                }
                const std::string r = test ? std::string("crashed (host killed by test)")
                                           : std::format("crashed ({})", child_.crashed() ? child_.terminationReason()
                                                                                         : std::format("exit code {}", child_.exitCode()));
                lk.unlock();
                response_.post(); // wake a waiting audio thread immediately
                reportCrash(typeId_, name_, r);
                setProblem(r);
                return;
            }
        }
    }

    void shutdown() {
        const int prev = state_.exchange(Stopped);
        stopWatch_ = true;
        if (watchdog_.joinable()) watchdog_.join();
        if (prev == Running) {
            std::lock_guard<std::mutex> lk(ioMutex_);
            const std::string s = json{{"cmd", "quit"}}.dump() + "\n";
            child_.writeAll(s.data(), s.size());
            child_.closeStdin();
            if (!child_.wait(2000)) child_.kill();
        } else {
            child_.kill();
        }
        request_.close();
        response_.close();
        if (block_) block_->~Block();
        block_ = nullptr;
        region_.close();
    }

    std::string typeId_, name_, shmName_;
    ipc::SharedRegion region_;
    ipc::Signal request_, response_;
    pipc::Block* block_ = nullptr;
    ChildProcess child_;
    std::mutex ioMutex_;
    mutable std::mutex reasonMutex_;
    std::string reason_;
    bool testKill_ = false;
    std::string controlTimeout_; // set under ioMutex_ before state_ becomes Hung
    std::atomic<int> state_{Running};
    std::atomic<bool> stopWatch_{false};
    std::thread watchdog_;
};

} // namespace plugins

// ------------------------------------------------------------------ processor
SandboxedPluginProcessor::SandboxedPluginProcessor(std::vector<ParamInfo> params) : Processor(std::move(params)) {}

SandboxedPluginProcessor::~SandboxedPluginProcessor() = default;

std::unique_ptr<SandboxedPluginProcessor> SandboxedPluginProcessor::create(const std::string& typeId, std::string* error) {
    std::string format, module, id;
    if (!plugins::parsePluginTypeId(typeId, format, module, id)) {
        if (error) *error = "not a plugin type id: " + typeId;
        return nullptr;
    }
    auto box = std::make_unique<plugins::Sandbox>();
    json info;
    std::string err;
    if (!box->launch(format, module, id, typeId, info, &err)) {
        log::error("plugins", "cannot load {}: {}", typeId, err);
        if (error) *error = err;
        return nullptr;
    }
    std::vector<ParamInfo> params;
    std::vector<uint32_t> ids;
    for (auto& p : info["params"]) {
        ParamInfo pi;
        pi.id = std::to_string(p["id"].get<uint32_t>());
        pi.name = p.value("name", pi.id);
        pi.minValue = p.value("min", 0.0f);
        pi.maxValue = p.value("max", 1.0f);
        pi.defaultValue = p.value("default", 0.0f);
        pi.steps = p.value("steps", p.value("stepped", false) ? static_cast<int>(pi.maxValue - pi.minValue) + 1 : 0);
        pi.unit = p.value("units", "");
        params.push_back(pi);
        ids.push_back(p["id"].get<uint32_t>());
    }
    std::unique_ptr<SandboxedPluginProcessor> proc(new SandboxedPluginProcessor(params));
    proc->typeId_ = typeId;
    proc->format_ = format;
    proc->name_ = info.value("name", id);
    for (size_t k = 0; k < ids.size(); ++k) proc->idToIndex_.push_back({ids[k], static_cast<int>(k)});
    std::sort(proc->idToIndex_.begin(), proc->idToIndex_.end());
    proc->instrument_ = info.value("instrument", false);
    proc->clapIds_ = ids;
    proc->lastSent_ = std::make_unique<float[]>(std::max<size_t>(1, ids.size()));
    size_t i = 0;
    for (auto& p : info["params"]) {
        const float v = p.value("value", p.value("default", 0.0f));
        proc->setParam(static_cast<int>(i), v);
        proc->lastSent_[i] = proc->getParam(static_cast<int>(i));
        ++i;
    }
    proc->box_ = std::move(box);
    log::info("plugins", "loaded {} ({}) in sandbox pid {}", proc->name_, typeId, proc->box_->pid());
    return proc;
}

void SandboxedPluginProcessor::prepare(double sampleRate, int maxBlockSize) {
    Processor::prepare(sampleRate, maxBlockSize);
    chunk_ = std::clamp(maxBlockSize, 1, static_cast<int>(pipc::kMaxFrames));
    json r;
    if (box_->call({{"cmd", "activate"}, {"sampleRate", sampleRate}, {"maxFrames", chunk_}}, r)) {
        latency_ = r.value("latency", 0);
        activated_ = true;
    } else {
        activated_ = false;
        log::error("plugins", "{}: activation failed", name_);
    }
}

void SandboxedPluginProcessor::reset() {
    json r;
    if (box_->state() == plugins::Sandbox::Running) box_->call({{"cmd", "reset"}}, r, 500);
}

void SandboxedPluginProcessor::process(const AudioBlock& io, const AudioBlock* sidechain, const NoteEvent* events, int numEvents) noexcept {
    if (!activated_.load(std::memory_order_relaxed) || box_->state() != plugins::Sandbox::Running) return; // bypass / silence
    pipc::Block* b = box_->block();
    const int timeoutUs = plugins::processTimeoutMs() * 1000;
    int ev = 0;
    for (int start = 0; start < io.numFrames; start += chunk_) {
        const int n = std::min(chunk_, io.numFrames - start);
        b->frames = static_cast<uint32_t>(n);
        for (int c = 0; c < 2; ++c) {
            const float* src = io.channel(std::min(c, io.numChannels - 1)) + start;
            std::memcpy(b->in[c], src, sizeof(float) * static_cast<size_t>(n));
        }
        uint32_t ne = 0;
        while (ev < numEvents && events[ev].offset < start + n) {
            const NoteEvent& e = events[ev++];
            if (ne >= pipc::kMaxEvents) continue;
            pipc::Event& o = b->events[ne++];
            o.offset = static_cast<uint32_t>(std::clamp(e.offset - start, 0, n - 1));
            o.type = static_cast<uint8_t>(e.type);
            o.channel = e.channel;
            o.note = e.note;
            o.velocity = e.velocity;
            o.value = e.value;
            o.controller = e.controller;
        }
        b->numEvents = ne;
        uint32_t np = 0;
        if (start == 0)
            for (size_t i = 0; i < clapIds_.size() && np < pipc::kMaxParamChanges; ++i) {
                const float v = getParam(static_cast<int>(i));
                if (v != lastSent_[i]) {
                    b->params[np++] = {0, clapIds_[i], static_cast<double>(v)};
                    lastSent_[i] = v;
                }
            }
        b->numParamChanges = np;
        b->numOutParams = 0;
        b->tempo = tempo_.load(std::memory_order_relaxed);
        b->transportFlags = 1;
        if (!box_->process(++seq_, timeoutUs)) {
            log::audioEvent(log::Level::Error, "plugin sandbox failed during process - plugin bypassed");
            return;
        }
        // plugin -> RoY parameter changes (edited in the plugin GUI, or output by the plugin)
        for (uint32_t k = 0; k < std::min(b->numOutParams, pipc::kMaxParamChanges); ++k) {
            const uint32_t pid = b->outParams[k].paramId;
            auto it = std::lower_bound(idToIndex_.begin(), idToIndex_.end(), std::make_pair(pid, -1));
            if (it == idToIndex_.end() || it->first != pid) continue;
            const float v = static_cast<float>(b->outParams[k].value);
            setParam(it->second, v);
            lastSent_[static_cast<size_t>(it->second)] = getParam(it->second); // do not echo it back
            lastTouched_.store(it->second, std::memory_order_relaxed);
            pluginEdits_.fetch_add(1, std::memory_order_relaxed);
        }
        for (int c = 0; c < io.numChannels && c < 2; ++c) {
            float* dst = io.channel(c) + start;
            if (instrument_)
                for (int i = 0; i < n; ++i) dst[i] += b->out[c][i];
            else
                std::memcpy(dst, b->out[c], sizeof(float) * static_cast<size_t>(n));
        }
    }
}

json SandboxedPluginProcessor::saveState() const {
    json j = Processor::saveState();
    json r;
    if (box_->state() == plugins::Sandbox::Running && box_->call({{"cmd", "state.save"}}, r)) {
        j["plugin"] = {{"format", format_}, {"state", r.value("state", "")}, {"pluginName", name_}};
        std::lock_guard<std::mutex> lk(stateMutex_);
        lastState_ = j;
        return j;
    }
    // Host crashed: keep the last good opaque state so nothing is lost.
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (lastState_.contains("plugin")) j["plugin"] = lastState_["plugin"];
    return j;
}

void SandboxedPluginProcessor::loadState(const json& state) {
    if (!state.is_object()) return;
    // "plugin" (current) or "clap" (projects saved by early alpha builds)
    const char* key = state.contains("plugin") ? "plugin" : "clap";
    if (state.contains(key) && state[key].is_object()) {
        json r;
        if (box_->call({{"cmd", "state.load"}, {"state", state[key].value("state", "")}}, r) && r.contains("values")) {
            for (size_t i = 0; i < clapIds_.size(); ++i) {
                auto key = std::to_string(clapIds_[i]);
                if (r["values"].contains(key)) {
                    setParam(static_cast<int>(i), r["values"][key].get<float>());
                    lastSent_[i] = getParam(static_cast<int>(i));
                }
            }
        } else {
            log::warn("plugins", "{}: plugin rejected the saved state", name_);
        }
        std::lock_guard<std::mutex> lk(stateMutex_);
        lastState_ = state;
        if (!lastState_.contains("plugin")) lastState_["plugin"] = state[key];
    }
    Processor::loadState(state);
}

bool SandboxedPluginProcessor::openEditor(bool alwaysOnTop, std::string* error) {
    json r;
    if (!box_->call({{"cmd", "editor.open"}, {"alwaysOnTop", alwaysOnTop}}, r, 10000)) {
        if (error) *error = r.is_object() ? r.value("error", std::string("editor could not be opened")) : "plugin host not responding";
        return false;
    }
    return true;
}

void SandboxedPluginProcessor::closeEditor() {
    json r;
    box_->call({{"cmd", "editor.close"}}, r);
}

json SandboxedPluginProcessor::editorState() const {
    json r;
    if (!box_->call({{"cmd", "editor.state"}}, r)) return {{"open", false}, {"available", false}};
    return r.value("editor", json::object());
}

bool SandboxedPluginProcessor::alive() const { return box_->state() == plugins::Sandbox::Running; }
std::string SandboxedPluginProcessor::problem() const { return alive() ? std::string() : box_->problem(); }
int SandboxedPluginProcessor::hostPid() const { return box_->pid(); }
void SandboxedPluginProcessor::killHostForTest() { box_->killForTest(); }

void registerPluginProcessors() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto creator = [](const std::string& typeId) -> std::unique_ptr<Processor> { return SandboxedPluginProcessor::create(typeId); };
        ProcessorFactory::instance().addPrefix("clap:", creator);
        ProcessorFactory::instance().addPrefix("vst3:", creator);
    });
}

} // namespace roy
