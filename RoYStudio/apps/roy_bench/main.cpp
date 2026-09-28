// roy_bench - performance benchmarks for RoY Studio.
//   roy_bench [--out <dir>] [--sizes 10,50,100,150] [--plugins <dir>] [--seconds N] [--baseline <benchmark.json>]
// With --baseline the results are compared with an earlier run; a CPU (mean) increase or an
// offline-render slowdown beyond the tolerance (default 15 %, --tolerance) is reported as
// REGRESSION and the exit code is 2 - regressions are never silent.
// Test projects (generated, MOCK content: synthetic audio/MIDI/patterns):
//   each "track group" = audio track (30 s clip, EQ + compressor), MIDI synth track (chords,
//   EQ), beat track (pattern, compressor); plus 4 busses with reverb/delay and a master chain.
// Measures per size: graph build, realtime callback CPU (mean / p99 / max vs buffer budget),
// blocks over budget (= xruns on a single audio thread), offline render speed, RAM,
// project save + load time. Optional: plugin scan time.
#include "audio/AudioEngine.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Process.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"
#include "project/ProjectIO.h"
#include "project/Session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace roy;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double rssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize / 1048576.0;
    return 0;
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atof(line.c_str() + 6) / 1024.0;
    return 0;
#endif
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Result {
    int tracks = 0;
    double buildMs = 0, meanLoad = 0, p99Load = 0, maxLoad = 0;
    int blocks = 0, overBudget = 0;
    double renderX = 0, rssMb = 0, saveMs = 0, loadMs = 0, projectKb = 0;
    int latency = 0;
    int workers = 0;
};

Result runSize(int groups, const fs::path& dir, double seconds, int buffer, int workers) {
    Result r;
    r.tracks = groups * 3;
    r.workers = workers;
    const double sr = 48000.0;
    AudioEngine engine;
    engine.prepare(sr, buffer);
    engine.setWorkerThreads(workers);
    ProjectRuntime rt(engine);
    Project p = makeNewProject(std::format("Bench {}", r.tracks), sr, 140.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt, dir};
    auto run = [&](const std::string& id, const json& a) {
        if (!reg.execute(ctx, id, a)) std::fprintf(stderr, "%s: %s\n", id.c_str(), ctx.error.c_str());
        return ctx.result;
    };
    // shared synthetic audio (30 s, stereo) registered once, used by every audio track
    auto data = std::make_shared<AudioData>();
    data->sampleRate = sr;
    data->numChannels = 2;
    data->numFrames = static_cast<int64_t>(30 * sr);
    data->channels.assign(2, std::vector<float>(static_cast<size_t>(data->numFrames)));
    for (int64_t i = 0; i < data->numFrames; ++i) {
        const double t = static_cast<double>(i) / sr;
        const float v = static_cast<float>(0.2 * std::sin(6.2831853 * 220 * t) * (0.6 + 0.4 * std::sin(6.2831853 * 0.5 * t)));
        data->channels[0][static_cast<size_t>(i)] = v;
        data->channels[1][static_cast<size_t>(i)] = v * 0.9f;
    }
    AudioAsset a;
    a.id = files::newId();
    a.path = "memory://bench"; // MOCK asset (in memory)
    a.originalName = "bench.wav";
    a.sampleRate = sr;
    a.channels = 2;
    a.frames = data->numFrames;
    p.assets.push_back(a);
    data->assetId = a.id;
    rt.addLoadedAsset(a.id, data);

    std::vector<std::string> busses;
    for (const char* b : {"Verb A", "Verb B", "Delay", "Parallel"}) busses.push_back(run("AddBus", {{"name", b}}).value("id", ""));
    run("AddInsert", {{"channelId", busses[0]}, {"typeId", "roy.reverb"}});
    run("AddInsert", {{"channelId", busses[1]}, {"typeId", "roy.reverb"}});
    run("AddInsert", {{"channelId", busses[2]}, {"typeId", "roy.delay"}});
    run("AddInsert", {{"channelId", busses[3]}, {"typeId", "roy.compressor"}});
    run("AddPattern", {{"name", "Beat"}});
    const std::string pat = ctx.result.value("id", "");
    run("SetRowPattern", {{"patternId", pat}, {"voice", "kick"}, {"text", "x...x...x...x..."}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "closed_hat"}, {"text", "xxxxxxxxxxxxxxxx"}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "snare"}, {"text", "....x.......x..."}});
    const double lenBeats = seconds * 140.0 / 60.0;
    for (int g = 0; g < groups; ++g) {
        auto t = run("AddTrack", {{"type", "audio"}, {"name", std::format("Audio {}", g)}});
        const std::string tid = t.value("id", ""), ch = t.value("channelId", "");
        run("AddAudioClip", {{"trackId", tid}, {"assetId", a.id}, {"startBeat", 0.0}, {"lengthBeats", std::min(lenBeats, 30 * 140.0 / 60.0)}});
        run("AddInsert", {{"channelId", ch}, {"typeId", "roy.eq"}});
        run("AddInsert", {{"channelId", ch}, {"typeId", "roy.compressor"}});
        run("AddSend", {{"channelId", ch}, {"target", busses[static_cast<size_t>(g % 4)]}, {"levelDb", -12.0}});
        auto m = run("AddTrack", {{"type", "midi"}, {"name", std::format("Synth {}", g)}});
        const std::string mid = m.value("id", "");
        run("AddInsert", {{"channelId", m.value("channelId", "")}, {"typeId", "roy.eq"}});
        run("AddMidiClip", {{"trackId", mid}, {"startBeat", 0.0}, {"lengthBeats", lenBeats}});
        const std::string clip = ctx.result.value("id", "");
        for (double b = 0; b < lenBeats; b += 4)
            for (int n : {57, 60, 64}) run("AddNote", {{"clipId", clip}, {"pitch", n + (g % 5)}, {"startBeat", b}, {"lengthBeats", 3.5}, {"wrongNoteMode", "off"}});
        auto d = run("AddTrack", {{"type", "beat"}, {"name", std::format("Beat {}", g)}});
        run("AddInsert", {{"channelId", d.value("channelId", "")}, {"typeId", "roy.compressor"}});
        run("AddPatternClip", {{"trackId", d.value("id", "")}, {"patternId", pat}, {"startBeat", 0.0}, {"lengthBeats", lenBeats}});
    }
    run("CreateMasterChain", {{"preset", "streaming"}});

    const auto b0 = Clock::now();
    if (!rt.rebuild(p)) std::fprintf(stderr, "rebuild failed\n");
    r.buildMs = ms(Clock::now() - b0);
    r.latency = rt.graphLatencySamples();

    // realtime simulation: time each callback against the buffer budget
    std::vector<float> l(static_cast<size_t>(buffer)), rr(static_cast<size_t>(buffer));
    float* outs[2] = {l.data(), rr.data()};
    engine.transport().play();
    const double budgetMs = 1000.0 * buffer / sr;
    const int blocks = static_cast<int>(seconds * sr / buffer);
    std::vector<double> loads;
    loads.reserve(static_cast<size_t>(blocks));
    for (int i = 0; i < 20; ++i) engine.process(nullptr, 0, outs, 2, buffer); // warm-up
    for (int i = 0; i < blocks; ++i) {
        const auto t0 = Clock::now();
        engine.process(nullptr, 0, outs, 2, buffer);
        loads.push_back(ms(Clock::now() - t0) / budgetMs);
    }
    engine.transport().stop();
    std::vector<double> sorted = loads;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double x : loads) sum += x;
    r.blocks = blocks;
    r.meanLoad = sum / std::max<size_t>(1, loads.size());
    r.p99Load = sorted[static_cast<size_t>(0.99 * (sorted.size() - 1))];
    r.maxLoad = sorted.back();
    r.overBudget = static_cast<int>(std::count_if(loads.begin(), loads.end(), [](double x) { return x > 1.0; }));

    // offline render speed
    OfflineRenderOptions o;
    o.numFrames = static_cast<int64_t>(seconds * sr);
    o.blockSize = 1024 > engine.maxBlockSize() ? engine.maxBlockSize() : 1024;
    const auto r0 = Clock::now();
    auto out = renderOffline(engine, o);
    r.renderX = seconds / std::max(1e-9, std::chrono::duration<double>(Clock::now() - r0).count());
    r.rssMb = rssMb();

    // save + load (the in-memory asset path is kept as-is)
    ProjectSession s;
    std::string err;
    const auto s0 = Clock::now();
    if (!s.create(dir, p, &err)) std::fprintf(stderr, "save: %s\n", err.c_str());
    r.saveMs = ms(Clock::now() - s0);
    const fs::path file = s.file();
    s.close();
    std::error_code ec;
    r.projectKb = static_cast<double>(fs::file_size(file, ec)) / 1024.0;
    Project q;
    ProjectSession s2;
    const auto l0 = Clock::now();
    if (!s2.open(file, q, OpenMode::Normal, &err)) std::fprintf(stderr, "load: %s\n", err.c_str());
    r.loadMs = ms(Clock::now() - l0);
    s2.close();
    return r;
}

// Cost of every registered processor: microseconds per block (stereo, 3 held notes for instruments).
std::string processorCosts(int buffer) {
    std::vector<std::pair<double, std::string>> rows;
    std::vector<float> l(static_cast<size_t>(buffer)), r(static_cast<size_t>(buffer));
    float* ch[2] = {l.data(), r.data()};
    for (auto& e : ProcessorFactory::instance().entries()) {
        auto p = e.create();
        p->prepare(48000.0, buffer);
        NoteEvent notes[3];
        for (int i = 0; i < 3; ++i) {
            notes[i].type = NoteEvent::NoteOn;
            notes[i].note = static_cast<int16_t>(e.typeId == "roy.drums" ? 36 + 2 * i : 57 + 3 * i);
            notes[i].velocity = 0.8f;
        }
        const int blocks = 2000;
        uint32_t seed = 1;
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < buffer; ++i) {
                seed = seed * 1664525u + 1013904223u;
                l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = e.instrument ? 0.0f : static_cast<float>(static_cast<int32_t>(seed)) / 4.0e9f;
            }
            AudioBlock blk{ch, 2, buffer};
            const bool retrigger = e.instrument && b % 94 == 0; // drums/808 re-hit every ~0.5 s
            p->process(blk, nullptr, retrigger ? notes : nullptr, retrigger ? 3 : 0);
        }
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / blocks;
        rows.push_back({us, std::format("| {} | {} | {:.1f} us | {:.2f} % |", e.typeId, e.instrument ? "instrument" : "effect", us,
                                        100.0 * us / (1e6 * buffer / 48000.0))});
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string md = "\n## Processor cost (one instance, per block)\n\n| Processor | Kind | Time/block | % of budget |\n|---|---|---|---|\n";
    for (auto& [us, line] : rows) md += line + "\n";
    return md;
}

} // namespace

int main(int argc, char** argv) {
    log::setLevel(log::Level::Error);
    registerBuiltinProcessors();
    fs::path outDir = fs::temp_directory_path() / "roy_bench";
    std::vector<int> sizes = {10, 50, 100, 150};
    fs::path pluginDir, baselineFile;
    double tolerance = 0.15;
    double seconds = 20.0;
    int buffer = 256;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--plugins" && i + 1 < argc) pluginDir = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--buffer" && i + 1 < argc) buffer = std::atoi(argv[++i]);
        else if (a == "--baseline" && i + 1 < argc) baselineFile = argv[++i];
        else if (a == "--tolerance" && i + 1 < argc) tolerance = std::atof(argv[++i]);
        else if (a == "--sizes" && i + 1 < argc) {
            sizes.clear();
            std::stringstream ss(argv[++i]);
            std::string x;
            while (std::getline(ss, x, ',')) sizes.push_back(std::atoi(x.c_str()));
        }
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    json results = json::array();
    std::string md = std::format("# RoY Studio benchmark {}\n\nHost: {} | buffer {} @ 48 kHz (budget {:.2f} ms) | {:.0f} s per size\n\n",
                                 files::nowIso8601(), hostName(), buffer, 1000.0 * buffer / 48000.0, seconds);
    const int maxWorkers = AudioEngine::defaultWorkerThreads();
    md += std::format("Machine: {} hardware threads -> {} mixing worker threads + the audio thread in multi-core mode.\n\n",
                      std::thread::hardware_concurrency(), maxWorkers);
    md += "| Size | Tracks | Threads | Graph build | CPU mean | CPU p99 | CPU max | Blocks over budget | Offline render | RAM | Save | Load | .roy size | PDC |\n";
    md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (int tracks : sizes) {
        const int groups = std::max(1, tracks / 3);
        for (int workers : {0, maxWorkers}) {
            if (workers == 0 && maxWorkers == 0 && !results.empty() && results.back().value("tracks", 0) == groups * 3) continue;
            const fs::path dir = outDir / std::format("size_{}_{}", tracks, workers);
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            std::fprintf(stderr, "benchmark %d tracks, %d workers...\n", groups * 3, workers);
            const Result r = runSize(groups, dir, seconds, buffer, workers);
            const char* name = tracks <= 10 ? "SMALL" : tracks <= 50 ? "MEDIUM" : tracks <= 100 ? "LARGE" : "XL";
            results.push_back({{"size", name}, {"tracks", r.tracks}, {"threads", workers + 1}, {"buildMs", r.buildMs}, {"cpuMean", r.meanLoad},
                               {"cpuP99", r.p99Load}, {"cpuMax", r.maxLoad}, {"blocks", r.blocks}, {"overBudget", r.overBudget},
                               {"renderRealtimeFactor", r.renderX}, {"rssMb", r.rssMb}, {"saveMs", r.saveMs}, {"loadMs", r.loadMs},
                               {"projectKb", r.projectKb}, {"latencySamples", r.latency}});
            md += std::format("| {} | {} (+4 busses) | {} | {:.0f} ms | {:.0f} % | {:.0f} % | {:.0f} % | {} / {} | {:.1f}x realtime | {:.0f} MB | {:.0f} ms | {:.0f} ms | {:.0f} KB | {} smp |\n",
                              name, r.tracks, workers + 1, r.buildMs, r.meanLoad * 100, r.p99Load * 100, r.maxLoad * 100, r.overBudget, r.blocks,
                              r.renderX, r.rssMb, r.saveMs, r.loadMs, r.projectKb, r.latency);
            if (maxWorkers == 0) break;
        }
    }
    if (!pluginDir.empty()) {
        plugins::PluginDatabase db;
        plugins::ScanOptions o;
        o.paths = {pluginDir};
        o.timeoutMs = 3000;
        const auto t0 = Clock::now();
        auto rep = plugins::scanPlugins(db, o);
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        md += std::format("\nPlugin scan: {} modules, {} plugins OK, {} crashed, {} timeouts (quarantined) in {:.2f} s "
                          "(out of process, the hang test plugin costs the full {} ms timeout)\n",
                          rep.modulesFound, rep.pluginsOk, rep.crashed, rep.timeouts, s, o.timeoutMs);
        results.push_back({{"pluginScan", rep.toJson()}, {"seconds", s}});
    }
    int regressions = 0;
    if (!baselineFile.empty()) {
        auto text = files::readAll(baselineFile);
        const json base = text ? json::parse(*text, nullptr, false) : json();
        md += std::format("\n## Comparison with baseline {}\n\nTolerance {:.0f} %. CPU values in % of the buffer budget; render in x realtime.\n\n",
                          baselineFile.filename().string(), tolerance * 100);
        if (!base.is_array()) {
            md += "Baseline could not be read - no comparison.\n";
        } else {
            md += "| Size | Threads | CPU mean (base -> now) | CPU p99 | Render | Verdict |\n|---|---|---|---|---|---|\n";
            for (auto& r : results) {
                if (!r.contains("size")) continue;
                const json* b = nullptr;
                for (auto& x : base)
                    if (x.value("size", std::string()) == r["size"].get<std::string>() && x.value("threads", 0) == r["threads"].get<int>()) b = &x;
                if (!b) continue;
                const double m0 = b->value("cpuMean", 0.0), m1 = r.value("cpuMean", 0.0);
                const double p0 = b->value("cpuP99", 0.0), p1 = r.value("cpuP99", 0.0);
                const double x0 = b->value("renderRealtimeFactor", 0.0), x1 = r.value("renderRealtimeFactor", 0.0);
                // absolute floor: changes below 2 % of the budget are measurement noise on a shared VM
                const bool cpuWorse = m1 > m0 * (1 + tolerance) && m1 - m0 > 0.02;
                const bool renderWorse = x0 > 0 && x1 < x0 / (1 + tolerance);
                const bool regressed = cpuWorse || renderWorse;
                regressions += regressed;
                md += std::format("| {} | {} | {:.0f} -> {:.0f} % | {:.0f} -> {:.0f} % | {:.1f} -> {:.1f}x | {} |\n", r["size"].get<std::string>(),
                                  r["threads"].get<int>(), m0 * 100, m1 * 100, p0 * 100, p1 * 100, x0, x1,
                                  regressed ? "REGRESSION" : (m1 < m0 / (1 + tolerance) ? "faster" : "ok"));
            }
            md += regressions ? std::format("\n**{} REGRESSION(S)** - investigate before release.\n", regressions) : "\nNo regression beyond the tolerance.\n";
        }
    }
    md += processorCosts(buffer);
    md += "\nCPU = callback wall time / buffer duration. Threads = audio thread + mixing workers (channels of one routing level run\n"
          "in parallel; output is bit-identical to single-threaded). Blocks over budget would be audible dropouts (xruns) on a\n"
          "real device with this buffer size. Measured on a shared cloud VM (no realtime scheduling), so p99/max include VM jitter.\n";
    files::atomicWrite(outDir / "benchmark.json", results.dump(2));
    files::atomicWrite(outDir / "benchmark.md", md);
    std::printf("%s", md.c_str());
    return regressions ? 2 : 0;
}
