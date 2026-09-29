// roy_soak - accelerated long-session soak test (MOCK session content: synthetic audio/MIDI/input).
//   roy_soak [--cycles N] [--plugin <file.clap>] [--out <dir>]
// Every cycle simulates about a minute of studio work, faster than realtime:
//   play 30 s (random seeks, loop on/off) -> record 10 s on an armed track -> edits (inserts, tracks,
//   notes, clip moves) -> undo/redo -> every 5th cycle a sandboxed plugin insert is added and removed
//   (process start/exit) -> every 10th cycle export 10 s WAV + save the session.
// After each cycle: RSS, open file descriptors, threads, undo memory. The report fits a line through
// the second half of the RSS samples: growth per simulated hour must stay small, fds/threads flat.
#include "audio/AudioEngine.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Process.h"
#include "export/Exporter.h"
#include "plugins/Sandbox.h"
#include "project/ProjectIO.h"
#include "project/Session.h"
#include "record/Recorder.h"
#include "record/Takes.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
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
int openFds() {
#ifdef _WIN32
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return static_cast<int>(n);
#else
    int n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator("/proc/self/fd", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
    return n;
#endif
}
int threadCount() {
#ifdef _WIN32
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    if (Thread32First(snap, &te))
        do n += te.th32OwnerProcessID == GetCurrentProcessId();
        while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("Threads:", 0) == 0) return std::atoi(line.c_str() + 8);
    return 0;
#endif
}
struct Sample {
    int cycle;
    double simMinutes, rss;
    int fds, threads;
    double undoMb;
    size_t tracks;
};
} // namespace

int main(int argc, char** argv) {
    log::setLevel(log::Level::Error);
    registerBuiltinProcessors();
    registerPluginProcessors();
    int cycles = 60;
    fs::path plugin, outDir = fs::temp_directory_path() / "roy_soak";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cycles" && i + 1 < argc) cycles = std::max(4, std::atoi(argv[++i]));
        else if (a == "--plugin" && i + 1 < argc) plugin = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    const double sr = 48000.0;
    const int block = 256;
    AudioEngine engine;
    engine.prepare(sr, block);
    engine.setWorkerThreads(AudioEngine::defaultWorkerThreads());
    ProjectRuntime rt(engine);
    Project p = makeNewProject("Soak", sr, 128.0);
    ProjectSession session;
    std::string err;
    if (!session.create(outDir, p, &err)) {
        std::fprintf(stderr, "session: %s\n", err.c_str());
        return 1;
    }
    const fs::path folder = session.folder();
    rt.setProjectDirectory(folder);
    UndoManager undo(p);
    undo.setLimit(200);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt, folder};
    int failures = 0;
    auto run = [&](const std::string& id, const json& a) {
        if (!reg.execute(ctx, id, a)) {
            ++failures;
            std::fprintf(stderr, "cycle command %s failed: %s\n", id.c_str(), ctx.error.c_str());
        }
        return ctx.result;
    };
    // session content: synthetic stems (MOCK), 4 groups of audio + synth + beat tracks, busses, master chain
    auto data = std::make_shared<AudioData>();
    data->sampleRate = sr;
    data->numChannels = 2;
    data->numFrames = static_cast<int64_t>(20 * sr);
    data->channels.assign(2, std::vector<float>(static_cast<size_t>(data->numFrames)));
    for (int64_t i = 0; i < data->numFrames; ++i)
        data->channels[0][static_cast<size_t>(i)] = data->channels[1][static_cast<size_t>(i)] =
            static_cast<float>(0.2 * std::sin(6.2831853 * 196.0 * static_cast<double>(i) / sr));
    AudioAsset asset;
    asset.id = files::newId();
    asset.path = "memory://soak";
    asset.originalName = "soak.wav";
    asset.sampleRate = sr;
    asset.channels = 2;
    asset.frames = data->numFrames;
    p.assets.push_back(asset);
    rt.addLoadedAsset(asset.id, data);
    const std::string bus = run("AddBus", {{"name", "FX"}}).value("id", "");
    run("AddInsert", {{"channelId", bus}, {"typeId", "roy.reverb"}});
    run("AddPattern", {{"name", "Loop"}});
    const std::string pat = ctx.result.value("id", "");
    run("GeneratePattern", {{"style", "Trap"}, {"seed", 3}});
    std::string vocalTrack;
    for (int g = 0; g < 4; ++g) {
        auto t = run("AddTrack", {{"type", "audio"}, {"name", std::format("Audio {}", g)}});
        if (g == 0) vocalTrack = t.value("id", "");
        run("AddAudioClip", {{"trackId", t.value("id", "")}, {"assetId", asset.id}, {"startBeat", 0.0}, {"lengthBeats", 32.0}});
        run("AddInsert", {{"channelId", t.value("channelId", "")}, {"typeId", "roy.compressor"}});
        run("AddSend", {{"channelId", t.value("channelId", "")}, {"target", bus}, {"levelDb", -10.0}});
        auto m = run("AddTrack", {{"type", "midi"}, {"name", std::format("Synth {}", g)}, {"instrument", "roy.synth"}});
        run("AddMidiClip", {{"trackId", m.value("id", "")}, {"startBeat", 0.0}, {"lengthBeats", 32.0}});
        const std::string clip = ctx.result.value("id", "");
        for (int b = 0; b < 32; b += 2) run("AddNote", {{"clipId", clip}, {"pitch", 60 + (b + g) % 12}, {"startBeat", b}, {"lengthBeats", 1.5}, {"wrongNoteMode", "off"}});
        auto d = run("AddTrack", {{"type", "beat"}, {"name", std::format("Beat {}", g)}});
        run("AddPatternClip", {{"trackId", d.value("id", "")}, {"patternId", pat}, {"startBeat", 0.0}, {"lengthBeats", 32.0}});
    }
    run("CreateMasterChain", {{"preset", "streaming"}});
    p.findTrack(vocalTrack)->armed = true;
    p.findTrack(vocalTrack)->inputLeft = 0;
    rt.rebuild(p);
    undo.clear();

    Recorder rec;
    rec.prepare(sr, block);
    rec.setOutputFolder(folder / "Audio");
    engine.setInputListener(&rec);

    std::vector<float> in(static_cast<size_t>(block)), l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    const float* ins[1] = {in.data()};
    float* outs[2] = {l.data(), r.data()};
    int64_t fed = 0;
    auto playSeconds = [&](double s) {
        const int blocks = static_cast<int>(s * sr / block);
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < block; ++i) in[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(6.2831853 * 330.0 * static_cast<double>(fed + i) / sr));
            engine.process(ins, 1, outs, 2, block);
            fed += block;
        }
    };
    std::mt19937 rng(42);
    std::vector<Sample> samples;
    std::vector<std::string> extraTracks;
    std::string lastTake, lastTakeTrack;
    const auto t0 = Clock::now();
    double simMinutes = 0;
    int takes = 0, exports = 0, pluginLoads = 0;
    for (int c = 1; c <= cycles; ++c) {
        // 1. playback with seeks and loop toggles
        engine.transport().seek(static_cast<int64_t>((rng() % 16) * sr / 4));
        run("SetLoop", {{"enabled", c % 2 == 0}, {"startBeat", 0.0}, {"endBeat", 16.0}});
        engine.transport().play();
        playSeconds(30.0);
        engine.transport().stop();
        playSeconds(0.5);
        // 2. record 10 s on the armed track, add the take, delete last cycle's take (its WAV stays, as in the app)
        rec.setTracks(takes::recordConfig(p));
        if (rec.startRecording(&err)) {
            engine.transport().play();
            playSeconds(10.0);
            rec.stopRecording();
            engine.transport().stop();
            playSeconds(0.5);
            rec.flush();
            for (auto& take : rec.collectFinishedTakes()) {
                undo.begin("Record Take");
                const std::string id = takes::addRecordedTake(p, take, folder);
                if (id.empty()) undo.cancel();
                else {
                    undo.end();
                    ++takes;
                    if (!lastTake.empty()) run("DeleteTake", {{"trackId", lastTakeTrack}, {"takeId", lastTake}});
                    lastTake = id;
                    lastTakeTrack = take.trackId;
                }
            }
        }
        // 3. edits + undo/redo
        const auto t = run("AddTrack", {{"type", "midi"}, {"name", std::format("Idea {}", c)}, {"instrument", "roy.synth"}});
        extraTracks.push_back(t.value("id", ""));
        run("AddInsert", {{"channelId", t.value("channelId", "")}, {"typeId", c % 3 ? "roy.eq" : "roy.delay"}});
        if (extraTracks.size() > 3) {
            run("DeleteTrack", {{"trackId", extraTracks.front()}});
            extraTracks.erase(extraTracks.begin());
        }
        run("SetPatternGroove", {{"patternId", pat}, {"groove", c % 2 ? "MPC 58%" : "Straight"}});
        run("SetChannelGain", {{"channelId", p.master()->id}, {"gainDb", -static_cast<double>(rng() % 3)}});
        for (int k = 0; k < 3; ++k) undo.undo();
        for (int k = 0; k < 2; ++k) undo.redo();
        rt.rebuild(p);
        // 4. sandboxed plugin in and out (process churn)
        if (!plugin.empty() && c % 5 == 0) {
            const std::string ch = p.findTrack(vocalTrack)->channelId;
            const auto res = run("AddInsert", {{"channelId", ch}, {"typeId", plugins::makeClapTypeId(plugin.string(), "com.roystudio.test.gain")}});
            rt.rebuild(p);
            engine.transport().play();
            playSeconds(2.0);
            engine.transport().stop();
            run("RemoveInsert", {{"slotId", res.value("id", "")}});
            rt.rebuild(p);
            ++pluginLoads;
        }
        // 5. export + save
        if (c % 10 == 0) {
            exporting::ExportOptions o;
            o.folder = outDir / "exports";
            o.baseName = std::format("soak_{}", c);
            o.range = exporting::Range::Selection;
            o.startBeat = 0;
            o.endBeat = 20;
            auto e = exporting::exportProject(engine, rt, p, o);
            if (!e.ok) {
                ++failures;
                std::fprintf(stderr, "export failed: %s\n", e.error.c_str());
            }
            for (auto& f : e.files) fs::remove(f.path, ec);
            ++exports;
            rt.captureProcessorStates(p);
            if (!session.save(p, &err)) {
                ++failures;
                std::fprintf(stderr, "save failed: %s\n", err.c_str());
            }
        }
        engine.collectGarbage();
        simMinutes += (30.0 + 10.0 + (plugin.empty() || c % 5 ? 0.0 : 2.0) + 20.0 /* editing time */) / 60.0;
        samples.push_back({c, simMinutes, rssMb(), openFds(), threadCount(), undo.memoryBytes() / 1048576.0, p.tracks.size()});
        if (c % 10 == 0)
            std::fprintf(stderr, "cycle %d: %.0f sim min, RSS %.1f MB, fds %d, threads %d, undo %.1f MB\n", c, simMinutes, samples.back().rss,
                         samples.back().fds, samples.back().threads, samples.back().undoMb);
    }
    engine.setInputListener(nullptr);
    session.close();
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    // linear fit of RSS over the second half (after warm-up / caches)
    const size_t from = samples.size() / 2;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = static_cast<double>(samples.size() - from);
    for (size_t i = from; i < samples.size(); ++i) {
        const double x = samples[i].simMinutes / 60.0, y = samples[i].rss;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / std::max(1e-12, n * sxx - sx * sx); // MB per simulated hour
    const auto& a0 = samples[from];
    const auto& a1 = samples.back();
    const bool rssOk = slope < 20.0;
    const bool fdOk = a1.fds <= a0.fds + 4;
    const bool thrOk = a1.threads <= a0.threads + 2;
    const bool pass = rssOk && fdOk && thrOk && failures == 0;
    std::string md = std::format("# RoY Studio soak test {}\n\n{} cycles = {:.1f} simulated hours of session work in {:.1f} min wall time "
                                 "({} takes recorded, {} exports, {} sandboxed plugin loads, {} worker threads).\n\n",
                                 files::nowIso8601(), cycles, simMinutes / 60.0, wall / 60.0, takes, exports, pluginLoads, engine.workerThreads());
    md += "| Check | Second half start | End | Verdict |\n|---|---|---|---|\n";
    md += std::format("| RSS | {:.1f} MB | {:.1f} MB | trend {:+.1f} MB per simulated hour → {} |\n", a0.rss, a1.rss, slope, rssOk ? "PASS" : "FAIL");
    md += std::format("| Open file descriptors / handles | {} | {} | {} |\n", a0.fds, a1.fds, fdOk ? "PASS" : "FAIL");
    md += std::format("| Threads | {} | {} | {} |\n", a0.threads, a1.threads, thrOk ? "PASS" : "FAIL");
    md += std::format("| Undo memory | {:.1f} MB | {:.1f} MB | limit 200 steps |\n", a0.undoMb, a1.undoMb);
    md += std::format("| Failed operations | | {} | {} |\n", failures, failures ? "FAIL" : "PASS");
    md += std::format("\n**Result: {}**\n\n## Samples\n\n| cycle | sim min | RSS MB | fds | threads | undo MB | tracks |\n|---|---|---|---|---|---|---|\n",
                      pass ? "PASS" : "FAIL");
    for (auto& s : samples)
        if (s.cycle % 5 == 0 || s.cycle == 1)
            md += std::format("| {} | {:.0f} | {:.1f} | {} | {} | {:.1f} | {} |\n", s.cycle, s.simMinutes, s.rss, s.fds, s.threads, s.undoMb, s.tracks);
    files::atomicWrite(outDir / "soak.md", md);
    std::printf("%s", md.c_str());
    return pass ? 0 : 2;
}
