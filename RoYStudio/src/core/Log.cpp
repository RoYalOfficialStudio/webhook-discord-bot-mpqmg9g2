#include "core/Log.h"
#include "core/SpscQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <regex>
#include <thread>

namespace roy::log {
namespace {

struct AudioEvent {
    Level level = Level::Info;
    const char* message = nullptr;
    double value = 0.0;
    int64_t timestampUs = 0;
};

struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::Info};
    bool console = true;
    FILE* file = nullptr;
    std::deque<std::string> recent;
    SpscQueue<AudioEvent> audioQueue{4096};
    std::atomic<uint64_t> audioDropped{0};
};

State& state() {
    static State s;
    return s;
}

int64_t nowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::string timestamp(int64_t us) {
    using namespace std::chrono;
    const auto tp = system_clock::time_point(microseconds(us));
    const auto secs = floor<seconds>(tp);
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", secs, static_cast<int>((us / 1000) % 1000));
}

void emitLine(Level l, std::string_view category, std::string_view msg, int64_t us) {
    auto& s = state();
    std::string line = std::format("{} [{}] [{}] {}", timestamp(us), levelName(l), category, msg);
    std::lock_guard lock(s.mutex);
    if (s.console) {
        FILE* out = l >= Level::Warn ? stderr : stdout;
        std::fprintf(out, "%s\n", line.c_str());
    }
    if (s.file) {
        std::fprintf(s.file, "%s\n", line.c_str());
        if (l >= Level::Warn) std::fflush(s.file);
    }
    s.recent.push_back(std::move(line));
    while (s.recent.size() > 1000) s.recent.pop_front();
}

} // namespace

const char* levelName(Level l) {
    switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    }
    return "?";
}

void setLevel(Level l) { state().level.store(l); }
Level level() { return state().level.load(std::memory_order_relaxed); }

void setConsole(bool enabled) {
    std::lock_guard lock(state().mutex);
    state().console = enabled;
}

bool setFile(const std::string& path) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) std::fclose(s.file);
    s.file = std::fopen(path.c_str(), "ab");
    return s.file != nullptr;
}

void closeFile() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) std::fclose(s.file);
    s.file = nullptr;
}

std::string redact(std::string_view msg) {
    static const std::regex kv(R"((token|password|passwd|secret|api[_-]?key|authorization)(\s*[=:]\s*)([^\s,;]+))",
                               std::regex::icase);
    static const std::regex bearer(R"((Bearer\s+)[A-Za-z0-9._~+/=-]+)", std::regex::icase);
    std::string out = std::regex_replace(std::string(msg), kv, "$1$2***");
    return std::regex_replace(out, bearer, "$1***");
}

void write(Level l, std::string_view category, std::string_view msg) {
    if (l < level()) return;
    emitLine(l, category, redact(msg), nowUs());
}

void audioEvent(Level l, const char* staticMessage, double value) {
    auto& s = state();
    // Several realtime threads (audio thread + mixing workers) may report: a tiny spin
    // lock around the single-producer ring keeps it correct without blocking syscalls.
    static std::atomic_flag producer = ATOMIC_FLAG_INIT;
    while (producer.test_and_set(std::memory_order_acquire)) {
    }
    const bool ok = s.audioQueue.push(AudioEvent{l, staticMessage, value, nowUs()});
    producer.clear(std::memory_order_release);
    if (!ok) s.audioDropped.fetch_add(1, std::memory_order_relaxed);
}

int flushAudioEvents() {
    auto& s = state();
    AudioEvent e;
    int n = 0;
    while (s.audioQueue.pop(e)) {
        ++n;
        if (e.level >= level())
            emitLine(e.level, "audio", std::format("{} ({})", e.message ? e.message : "?", e.value), e.timestampUs);
    }
    return n;
}

uint64_t droppedAudioEvents() { return state().audioDropped.load(); }

std::string recentLines(int maxLines) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::string out;
    const int start = std::max(0, static_cast<int>(s.recent.size()) - maxLines);
    for (size_t i = static_cast<size_t>(start); i < s.recent.size(); ++i) {
        out += s.recent[i];
        out += '\n';
    }
    return out;
}

} // namespace roy::log
