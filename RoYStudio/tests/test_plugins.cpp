// Plugin system: out-of-process scanner (quarantine, VST3 detection, database views)
// and the sandboxed CLAP host (audio via shared memory, crash / hang isolation).
// Uses REAL CLAP plugins built from plugins_test/ (test plugins, clearly labelled).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "core/Log.h"
#include "core/Process.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"

#include <cstdlib>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <csignal>
#endif

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;
const fs::path kPluginDir = ROY_TEST_PLUGIN_DIR;
const std::string kGain = (kPluginDir / "roy_test_gain.clap").string();
const std::string kCrash = (kPluginDir / "roy_test_crash.clap").string();
const std::string kHang = (kPluginDir / "roy_test_hang.clap").string();

void setupHost() {
    plugins::setHostExecutable(ROY_PLUGIN_HOST_EXE);
    registerBuiltinProcessors();
    registerPluginProcessors();
}

void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

// Audio track with a sine clip (+ optional insert on its channel).
struct SineProject {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("plug", SR, 120.0);
    std::string trackId, channelId;
    SineProject() {
        engine.prepare(SR, 256);
        auto d = makeSine(SR, 220.0, 4.0, 0.5f);
        const std::string asset = addMemoryAsset(p, rt, d);
        Track& t = addTrack(p, TrackType::Audio, "Sine");
        trackId = t.id;
        channelId = t.channelId;
        addClip(*p.findTrack(trackId), asset, 0, 8.0);
    }
    std::vector<std::vector<float>> run(int64_t frames) {
        REQUIRE(rt.rebuild(p));
        return render(engine, frames, 256);
    }
};
} // namespace

TEST_CASE("plugins", "scanner: CLAP detection, categories, database views, rescan, duplicates") {
    setupHost();
    const fs::path dir = tempDir("plugscan");
    fs::create_directories(dir / "sub");
    fs::copy_file(kGain, dir / "gain.clap");
    fs::copy_file(kGain, dir / "sub" / "gain_copy.clap"); // duplicate install
    plugins::PluginDatabase db;
    plugins::ScanOptions opt;
    opt.paths = {dir};
    opt.hostExe = ROY_PLUGIN_HOST_EXE;
    auto rep = plugins::scanPlugins(db, opt);
    CHECK(rep.modulesFound == 2);
    CHECK(rep.pluginsOk == 4);
    CHECK(rep.crashed == 0);
    CHECK(rep.duplicates == 2);
    CHECK(db.instruments().size() == 2);
    CHECK(db.effects().size() == 2);
    const std::string gainType = plugins::makeClapTypeId((dir / "gain.clap").string(), "com.roystudio.test.gain");
    const auto* g = db.find(gainType);
    REQUIRE(g != nullptr);
    CHECK(g->name == "RoY Test Gain");
    CHECK(g->vendor == "RoY Studio (test)");
    CHECK(g->version == "1.2.0");
    CHECK(g->category == "effect");
    CHECK(g->paramCount == 1);
    CHECK(g->arch == plugins::hostArch());
    CHECK(g->duplicateOf.empty() != db.find(plugins::makeClapTypeId((dir / "sub" / "gain_copy.clap").string(), "com.roystudio.test.gain"))->duplicateOf.empty());

    // favorites / recent / blacklist views
    db.setFavorite(gainType, true);
    db.markUsed(gainType);
    CHECK(db.view("FAVORITES").size() == 1);
    CHECK(db.view("RECENT").size() == 1);
    db.setBlacklisted((dir / "sub" / "gain_copy.clap").string(), true);
    CHECK(db.view("BLACKLISTED").size() == 2);
    CHECK(db.view("AVAILABLE").size() == 2);
    CHECK(db.view("INSTALLED").size() == 4);

    // persistence
    const fs::path dbFile = dir / "db" / "plugins.json";
    REQUIRE(db.save(dbFile));
    plugins::PluginDatabase db2;
    REQUIRE(db2.load(dbFile));
    CHECK(db2.records.size() == db.records.size());
    CHECK(db2.favorites.count(gainType) == 1);
    CHECK(db2.isBlacklisted((dir / "sub" / "gain_copy.clap").string()));

    // rescan skips unchanged modules and blacklisted ones
    auto rep2 = plugins::scanPlugins(db2, opt);
    CHECK(rep2.scanned == 0);
    CHECK(rep2.skippedUnchanged == 1);
    CHECK(rep2.skippedBlacklisted == 1);
    CHECK(db2.records.size() == 4);
    opt.force = true;
    auto rep3 = plugins::scanPlugins(db2, opt);
    CHECK(rep3.scanned == 1);
    CHECK(db2.records.size() == 4);
}

TEST_CASE("plugins", "scanner: crash and hang are quarantined, VST3 detected, wrong architecture rejected") {
    setupHost();
    const fs::path dir = tempDir("plugquar");
    fs::copy_file(kCrash, dir / "crash.clap");
    fs::copy_file(kHang, dir / "hang.clap");
    // VST3 bundle with a moduleinfo.json (comments + trailing commas like real files)
    const fs::path res = dir / "Fake Synth.vst3" / "Contents" / "Resources";
    fs::create_directories(res);
    std::ofstream(res / "moduleinfo.json") << R"({
  // generated
  "Name": "Fake Synth",
  "Factory Info": {"Vendor": "Example Vendor",},
  "Classes": [
    {"CID": "ABCDEF0123456789ABCDEF0123456789", "Category": "Audio Module Class", "Name": "Fake Synth",
     "Version": "2.0.1", "Sub Categories": ["Instrument", "Synth",],},
    {"CID": "00", "Category": "Component Controller Class", "Name": "Fake Synth Controller"},
  ],
})";
    // 32-bit Windows DLL header renamed to .clap -> wrong architecture
    {
        std::ofstream f(dir / "old32.clap", std::ios::binary);
        std::string h(0x80, '\0');
        h[0] = 'M';
        h[1] = 'Z';
        h[0x3C] = 0x40;
        h[0x40] = 'P';
        h[0x41] = 'E';
        h[0x44] = 0x4c;
        h[0x45] = 0x01;
        f << h;
    }
    plugins::PluginDatabase db;
    plugins::ScanOptions opt;
    opt.paths = {dir};
    opt.hostExe = ROY_PLUGIN_HOST_EXE;
    opt.timeoutMs = 1500;
    setEnv("ROY_TEST_CRASH_AT_SCAN", "1");
    auto rep = plugins::scanPlugins(db, opt);
    setEnv("ROY_TEST_CRASH_AT_SCAN", nullptr);
    CHECK(rep.modulesFound == 4);
    CHECK(rep.crashed == 1);
    CHECK(rep.timeouts == 1);
    CHECK(rep.unsupported == 1);
    CHECK(db.quarantine.count((dir / "crash.clap").string()) == 1);
    CHECK(db.quarantine.count((dir / "hang.clap").string()) == 1);
    CHECK(db.view("FAILED").size() == 4);
    bool vst = false, wrong = false;
    for (auto& r : db.records) {
        if (r.format == "vst3") {
            vst = true;
            CHECK(r.name == "Fake Synth");
            CHECK(r.vendor == "Example Vendor");
            CHECK(r.version == "2.0.1");
            CHECK(r.category == "instrument");
            CHECK(r.status == "unsupported");
        }
        if (r.status == "wrong_arch") {
            wrong = true;
            CHECK(r.arch == "x86");
        }
        if (r.status == "crashed") CHECK(r.error.find("crashed") != std::string::npos);
    }
    CHECK(vst);
    CHECK(wrong);
    CHECK(fs::exists(dir / "crash.clap")); // never moved or deleted

    // quarantined modules are skipped until the user retries them
    auto rep2 = plugins::scanPlugins(db, opt);
    CHECK(rep2.skippedQuarantined == 2);
    CHECK(rep2.crashed == 0);
    opt.retryQuarantined = true;
    opt.paths = {dir / "crash.clap"};
    auto rep3 = plugins::scanPlugins(db, opt); // no crash env now -> it loads
    CHECK(rep3.pluginsOk == 1);
    CHECK(db.quarantine.count((dir / "crash.clap").string()) == 0);
    CHECK(db.quarantine.count((dir / "hang.clap").string()) == 1);
}

TEST_CASE("plugins", "sandboxed CLAP effect processes audio in a separate process, state round-trips") {
    setupHost();
    const std::string type = plugins::makeClapTypeId(kGain, "com.roystudio.test.gain");
    SineProject ref;
    auto dry = ref.run(48000);

    SineProject s;
    UndoManager undo(s.p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{s.p, undo, &s.rt};
    REQUIRE(reg.execute(ctx, "AddInsert", {{"channelId", s.channelId}, {"typeId", type}, {"name", "Test Gain"}, {"params", {{"0", 0.5}}}}));
    const std::string slot = ctx.result["id"];
    auto wet = s.run(48000);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    CHECK(proc->alive());
    CHECK(proc->hostPid() > 0 && proc->hostPid() != currentProcessId());
    CHECK(proc->displayName() == "RoY Test Gain");
    CHECK(!proc->isInstrument());
    REQUIRE(proc->numParams() == 1);
    CHECK(proc->paramInfo(0).name == "Gain");
    double maxErr = 0;
    for (size_t i = 0; i < dry[0].size(); ++i) maxErr = std::max(maxErr, static_cast<double>(std::fabs(wet[0][i] - 0.5f * dry[0][i])));
    CHECK_MSG(maxErr < 1e-6, std::format("max error {}", maxErr));
    CHECK(rms(dry[0]) > 0.1);

    // parameter change (automation path) reaches the plugin on the next block
    proc->setParam(0, 2.0f);
    auto loud = render(s.engine, 4800, 256);
    CHECK_NEAR(rms(loud[0], 1000, 4800) / rms(dry[0], 1000, 4800), 2.0, 1e-3);

    // opaque plugin state is saved into the project and restored into a new instance
    s.rt.captureProcessorStates(s.p);
    const PluginSlot& ps = s.p.findChannel(s.channelId)->inserts[0];
    REQUIRE(ps.state.contains("clap"));
    CHECK(!ps.state["clap"]["state"].get<std::string>().empty());
    auto fresh = SandboxedPluginProcessor::create(type);
    REQUIRE(fresh != nullptr);
    CHECK_NEAR(fresh->getParam(0), 1.0, 1e-9);
    fresh->loadState(ps.state);
    CHECK_NEAR(fresh->getParam(0), 2.0, 1e-9);
    // capturing does not force a new plugin process on the next rebuild
    const int pid = proc->hostPid();
    REQUIRE(s.rt.rebuild(s.p));
    CHECK(std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->hostPid() == pid);

    // no heap allocation in the DAW-side process() call
    std::vector<float> l(256, 0.1f), r(256, 0.1f);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock blk{ch, 2, 256};
    long allocs;
    {
        AllocationCounter counter;
        proc->process(blk, nullptr, nullptr, 0);
        allocs = counter.count();
    }
    CHECK(allocs == 0);
    CHECK_NEAR(l[10], 0.2f, 1e-6);
}

TEST_CASE("plugins", "sandboxed CLAP instrument plays MIDI notes") {
    setupHost();
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("inst", SR, 120.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt};
    const std::string tid = addTrack(p, TrackType::Midi, "Plugin Synth").id;
    REQUIRE(reg.execute(ctx, "SetInstrument", {{"trackId", tid}, {"typeId", plugins::makeClapTypeId(kGain, "com.roystudio.test.sine")}}));
    MidiClip mc;
    mc.id = files::newId();
    mc.startBeat = 0;
    mc.lengthBeats = 4;
    mc.notes = {{69, 127, 0.0, 1.0}}; // A4 for 0.5 s
    p.findTrack(tid)->midiClips.push_back(mc);
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 48000, 256);
    auto inst = std::dynamic_pointer_cast<SandboxedPluginProcessor>(rt.processorForSlot(p.findTrack(tid)->instrument->id));
    REQUIRE(inst != nullptr);
    CHECK(inst->isInstrument());
    // count zero crossings in 0.1..0.4 s -> 440 Hz
    int zc = 0;
    for (size_t i = 4800; i < 19200; ++i)
        if ((out[0][i - 1] < 0) != (out[0][i] < 0)) ++zc;
    const double hz = zc / 2.0 / 0.3;
    CHECK_MSG(std::fabs(hz - 440.0) < 5.0, std::format("{} Hz", hz));
    CHECK(rms(out[0], 4800, 19200) > 0.05);
    CHECK(rms(out[0], 30000, 48000) < 1e-4); // note off
}

TEST_CASE("plugins", "plugin crash during playback: PLUGIN CRASHED, project continues, restart recovers") {
    setupHost();
    plugins::takeCrashEvents();
    const fs::path crashDir = tempDir("plugcrash");
    plugins::setCrashReportDirectory(crashDir.string());
    SineProject ref;
    auto dry = ref.run(48000);

    SineProject s;
    UndoManager undo(s.p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{s.p, undo, &s.rt};
    REQUIRE(reg.execute(ctx, "AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeClapTypeId(kCrash, "com.roystudio.test.crash")}}));
    const std::string slot = ctx.result["id"];
    auto out = s.run(48000); // the plugin segfaults after 20 blocks (~0.1 s)
    REQUIRE(out[0].size() == dry[0].size());
    CHECK(allFinite(out[0]));
    // after the crash the channel passes audio through unchanged (bypass)
    double err = 0;
    for (size_t i = 24000; i < 48000; ++i) err = std::max(err, static_cast<double>(std::fabs(out[0][i] - dry[0][i])));
    CHECK_MSG(err < 1e-6, std::format("post-crash error {}", err));
    auto crashed = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    for (int i = 0; i < 100 && crashed->problem().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(reg.execute(ctx, "PluginStatus", json::object()));
    REQUIRE(ctx.result["plugins"].size() == 1);
    CHECK(ctx.result["plugins"][0]["status"] == "PLUGIN CRASHED");
    CHECK(ctx.result["plugins"][0]["problem"].get<std::string>().find("crashed") != std::string::npos);
    CHECK(ctx.result["newCrashes"].size() == 1);
    CHECK(log::recentLines(400).find("PLUGIN CRASHED") != std::string::npos);
    bool report = false;
    for (auto& e : fs::directory_iterator(crashDir)) report |= e.path().filename().string().rfind("plugin_crash_", 0) == 0;
    CHECK(report);

    // recovery: a fresh sandbox instance
    REQUIRE(reg.execute(ctx, "RestartPlugin", {{"slotId", slot}}));
    CHECK(ctx.result["restarted"] == true);
    auto again = render(s.engine, 2560, 256); // 10 blocks: before the next scripted crash
    CHECK(std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->alive());
    CHECK(allFinite(again[0]));
    plugins::setCrashReportDirectory("");
}

#ifndef _WIN32
TEST_CASE("plugins", "hung plugin host is detected by timeout and the audio continues") {
    setupHost();
    const int oldTimeout = plugins::processTimeoutMs();
    plugins::setProcessTimeoutMs(100);
    SineProject s;
    PluginSlot slot;
    slot.id = files::newId();
    slot.typeId = plugins::makeClapTypeId(kGain, "com.roystudio.test.gain");
    slot.name = "gain";
    slot.state["params"] = {{"0", 0.5}};
    s.p.findChannel(s.channelId)->inserts.push_back(slot);
    auto first = s.run(4800);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot.id));
    REQUIRE(proc && proc->alive());
    ::kill(proc->hostPid(), SIGSTOP); // freeze the host: it never answers again
    const auto t0 = std::chrono::steady_clock::now();
    auto out = render(s.engine, 4800, 256);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!proc->alive());
    CHECK_MSG(secs < 1.5, std::format("render took {} s", secs)); // one timeout (reset or audio), then bypass
    CHECK(allFinite(out[0]));
    CHECK(rms(out[0], 2400, 4800) > 0.3); // bypassed: full-level dry sine (0.5 amp)
    for (int i = 0; i < 100 && proc->problem().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(proc->problem().find("hung") != std::string::npos);
    plugins::setProcessTimeoutMs(oldTimeout);
}
#endif

TEST_CASE("plugins", "missing plugin module is reported, project still builds") {
    setupHost();
    SineProject s;
    PluginSlot slot;
    slot.id = files::newId();
    slot.typeId = plugins::makeClapTypeId("/nonexistent/none.clap", "x.y");
    slot.name = "gone";
    s.p.findChannel(s.channelId)->inserts.push_back(slot);
    auto out = s.run(4800);
    CHECK(rms(out[0], 1000, 4800) > 0.3);
    bool warned = false;
    for (auto& w : s.rt.lastWarnings()) warned |= w.find("not available") != std::string::npos;
    CHECK(warned);
}
