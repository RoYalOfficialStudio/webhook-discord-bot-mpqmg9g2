// VST3 hosting (sandboxed): scan -> load -> instantiate -> process audio -> MIDI -> automation ->
// state save/restore (component + controller) -> editor open/resize/close -> crash -> unload.
// Also the CLAP editor lifecycle. Uses the RoY VST3/CLAP TEST plugins built from plugins_test/.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "core/Log.h"
#include "core/Process.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"
#include "project/ProjectIO.h"
#include "project/Session.h"

#include <cstdlib>
#include <thread>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;
const fs::path kBundle = fs::path(ROY_TEST_PLUGIN_DIR) / "RoYTest.vst3";
const std::string kGainCid = "524F590147414950524F433100000001";
const std::string kSynthCid = "524F590253594E50524F433100000003";
const std::string kCrashCid = "524F590343524150524F433100000005";

void setup() {
    plugins::setHostExecutable(ROY_PLUGIN_HOST_EXE);
    registerBuiltinProcessors();
    registerPluginProcessors();
}

bool haveDisplay() {
#ifdef _WIN32
    return true;
#else
    const char* d = std::getenv("DISPLAY");
    return d && *d;
#endif
}

struct Proj {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("vst3", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string trackId, channelId;
    Proj() {
        engine.prepare(SR, 256);
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        auto d = makeSine(SR, 220.0, 6.0, 0.5f);
        const std::string asset = addMemoryAsset(p, rt, d);
        Track& t = addTrack(p, TrackType::Audio, "Sine");
        trackId = t.id;
        channelId = t.channelId;
        addClip(*p.findTrack(trackId), asset, 0, 12.0);
    }
    bool run(const std::string& id, const json& a) { return reg.execute(*ctx, id, a); }
    std::vector<std::vector<float>> render(int64_t frames) {
        REQUIRE(rt.rebuild(p));
        return roytest::render(engine, frames, 256);
    }
};
} // namespace

TEST_CASE("vst3", "scan: VST3 bundle scanned out of process, classes instantiated") {
    setup();
    const fs::path dir = tempDir("vst3scan");
    fs::copy(kBundle, dir / "RoYTest.vst3", fs::copy_options::recursive);
    plugins::PluginDatabase db;
    plugins::ScanOptions o;
    o.paths = {dir};
    o.hostExe = ROY_PLUGIN_HOST_EXE;
    auto rep = plugins::scanPlugins(db, o);
    CHECK(rep.modulesFound == 1);
    CHECK(rep.pluginsOk == 4);
    CHECK(rep.unsupported == 0);
    const auto* g = db.find(plugins::makeVst3TypeId((dir / "RoYTest.vst3").string(), kGainCid));
    REQUIRE(g != nullptr);
    CHECK(g->format == "vst3");
    CHECK(g->name == "RoY VST3 Gain");
    CHECK(g->vendor == "RoY Studio (test)");
    CHECK(g->version == "1.3.0");
    CHECK(g->category == "effect");
    CHECK(g->paramCount == 3);
    CHECK(db.instruments().size() == 1);
    CHECK(db.effects().size() == 3);
}

TEST_CASE("vst3", "effect: load, process audio, parameters, automation") {
    setup();
    Proj ref;
    auto dry = ref.render(48000);
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kGainCid)}, {"name", "VST3 Gain"},
                                {"params", {{"0", 0.25}}}}));
    const std::string slot = s.ctx->result["id"];
    auto wet = s.render(48000);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    CHECK(proc->format() == "vst3");
    CHECK(proc->hostPid() != currentProcessId());
    REQUIRE(proc->numParams() == 3);
    CHECK(proc->paramInfo(0).name == "Gain");
    CHECK(proc->paramInfo(1).name == "Bypass");
    CHECK(proc->paramInfo(1).steps == 2);
    double maxErr = 0;
    for (size_t i = 0; i < dry[0].size(); ++i) maxErr = std::max(maxErr, static_cast<double>(std::fabs(wet[0][i] - 0.5f * dry[0][i])));
    CHECK_MSG(maxErr < 1e-6, std::format("gain error {}", maxErr)); // normalized 0.25 -> x0.5
    // bypass parameter
    REQUIRE(s.run("SetParam", {{"slotId", slot}, {"paramId", "1"}, {"value", 1.0}}));
    auto by = roytest::render(s.engine, 4800, 256);
    CHECK_NEAR(rms(by[0], 1000, 4800) / rms(dry[0], 1000, 4800), 1.0, 1e-3);
    REQUIRE(s.run("SetParam", {{"slotId", slot}, {"paramId", "1"}, {"value", 0.0}}));
    // automation of a plugin parameter: gain ramps 0 -> 1 (x0 -> x2) over 4 beats
    REQUIRE(s.run("CreateAutomation", {{"channelId", s.channelId}, {"slotId", slot}, {"paramId", "0"}, {"points", {{0.0, 0.0}, {4.0, 1.0}}}}));
    auto aut = s.render(144000); // 4 beats @ 120 BPM = 2 s = 96000 samples
    const double early = rms(aut[0], 256, 2400), mid = rms(aut[0], 45600, 50400), late = rms(aut[0], 100000, 140000);
    const double base = rms(dry[0], 24000, 46000);
    CHECK(early < 0.1 * base);
    CHECK_NEAR(mid / base, 1.0, 0.05); // halfway: normalized 0.5 -> x1.0
    CHECK_NEAR(late / base, 2.0, 0.02);
}

TEST_CASE("vst3", "instrument: MIDI notes -> audio") {
    setup();
    Proj s;
    const std::string tid = addTrack(s.p, TrackType::Midi, "VST3 Synth").id;
    REQUIRE(s.run("SetInstrument", {{"trackId", tid}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kSynthCid)}}));
    MidiClip mc;
    mc.id = files::newId();
    mc.lengthBeats = 4;
    mc.notes = {{69, 127, 0.0, 1.0}};
    s.p.findTrack(tid)->midiClips.push_back(mc);
    s.p.findTrack(s.trackId)->audioClips.clear(); // only the synth
    auto out = s.render(48000);
    auto inst = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(s.p.findTrack(tid)->instrument->id));
    REQUIRE(inst != nullptr);
    CHECK(inst->isInstrument());
    int zc = 0;
    for (size_t i = 4800; i < 19200; ++i)
        if ((out[0][i - 1] < 0) != (out[0][i] < 0)) ++zc;
    CHECK_MSG(std::fabs(zc / 2.0 / 0.3 - 440.0) < 5.0, std::format("{} Hz", zc / 0.6));
    CHECK(rms(out[0], 4800, 19200) > 0.05);
    CHECK(rms(out[0], 30000, 48000) < 1e-4); // note off reached the plugin
}

TEST_CASE("vst3", "state: component + controller state saved in the project and restored after reopen") {
    setup();
    const fs::path dir = tempDir("vst3state");
    std::string err, slot, blob;
    fs::path file;
    {
        Proj s;
        ProjectSession session;
        REQUIRE(session.create(dir, s.p, &err));
        file = session.file();
        REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kGainCid)}}));
        slot = s.ctx->result["id"];
        REQUIRE(s.run("SetParam", {{"slotId", slot}, {"paramId", "0"}, {"value", 0.8}}));
        s.render(4800);
        auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
        REQUIRE(proc != nullptr);
        s.rt.captureProcessorStates(s.p);
        blob = s.p.findSlot(slot)->state["plugin"]["state"].get<std::string>(); // opaque plugin chunk (format-neutral key)
        CHECK(!blob.empty());
        REQUIRE(session.save(s.p, &err));
        session.close();
    }
    Project q;
    ProjectSession s2;
    REQUIRE(s2.open(file, q, OpenMode::Normal, &err));
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    // the sine clip was an in-memory mock asset: replace it with a real one for the reopened render
    auto d = makeSine(SR, 220.0, 2.0, 0.5f);
    rt.addLoadedAsset(q.assets[0].id, d);
    REQUIRE(rt.rebuild(q));
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    CHECK_NEAR(proc->getParam(0), 0.8, 1e-6);
    // saving again yields the identical chunk: component AND controller state were restored
    const json again = proc->saveState();
    CHECK(again["plugin"]["state"].get<std::string>() == blob);
    auto out = roytest::render(engine, 9600, 256);
    CHECK_NEAR(rms(out[0], 2000, 9600) / rms(d->channels[0], 2000, 9600), 1.6, 0.01);
    s2.close();
}

TEST_CASE("vst3", "editor: open, plugin-requested resize, GUI edit reaches RoY, close keeps the plugin") {
    if (!haveDisplay()) {
        std::printf("       (skipped: no X11 display - run the suite under xvfb-run)\n");
        return;
    }
    setup();
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kGainCid)}}));
    const std::string slot = s.ctx->result["id"];
    s.render(2400);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    std::string err;
    REQUIRE_MSG_OK(proc->openEditor(false, &err), err);
    json st = proc->editorState();
    CHECK(st.value("open", false));
    CHECK(st.value("width", 0) == 420); // the view asked the host frame to resize (IPlugFrame::resizeView)
    CHECK(st.value("height", 0) == 320);
    CHECK(st.value("resizeRequests", 0) >= 1);
    // the editor's "knob move" (performEdit) arrives in RoY on the next processed block
    const int touch = proc->findParam("2");
    REQUIRE(touch >= 0);
    roytest::render(s.engine, 2560, 256);
    CHECK_NEAR(proc->getParam(touch), 0.75, 1e-6);
    CHECK(proc->lastTouchedParam() == touch);
    proc->closeEditor();
    CHECK(!proc->editorState().value("open", true));
    CHECK(proc->alive());
    auto after = roytest::render(s.engine, 4800, 256);
    CHECK(rms(after[0], 1000, 4800) > 0.3); // still processing after the editor closed
    REQUIRE(proc->openEditor(true, &err)); // reopen (always on top)
    CHECK(proc->editorState().value("open", false));
    proc->closeEditor();
}

TEST_CASE("vst3", "clap editor: gui extension, host resize, timers, output events") {
    if (!haveDisplay()) {
        std::printf("       (skipped: no X11 display - run the suite under xvfb-run)\n");
        return;
    }
    setup();
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId},
                                {"typeId", plugins::makeClapTypeId((fs::path(ROY_TEST_PLUGIN_DIR) / "roy_test_gain.clap").string(), "com.roystudio.test.gain")}}));
    const std::string slot = s.ctx->result["id"];
    s.render(2400);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    std::string err;
    REQUIRE_MSG_OK(proc->openEditor(false, &err), err);
    json st = proc->editorState();
    CHECK(st.value("open", false));
    CHECK(st.value("width", 0) == 320); // clap host_gui.request_resize
    CHECK(st.value("height", 0) == 240);
    roytest::render(s.engine, 2560, 256);
    const int touch = proc->findParam("1");
    REQUIRE(touch >= 0);
    CHECK_NEAR(proc->getParam(touch), 0.75, 1e-6); // clap output param event -> RoY
    CHECK(proc->lastTouchedParam() == touch);
    proc->closeEditor();
    CHECK(!proc->editorState().value("open", true));
    CHECK(proc->alive());
}

TEST_CASE("vst3", "crash during processing: PLUGIN CRASHED, pass-through, restart") {
    setup();
    plugins::takeCrashEvents();
    Proj ref;
    auto dry = ref.render(48000);
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kCrashCid)}}));
    const std::string slot = s.ctx->result["id"];
    auto out = s.render(48000);
    double err = 0;
    for (size_t i = 24000; i < 48000; ++i) err = std::max(err, static_cast<double>(std::fabs(out[0][i] - dry[0][i])));
    CHECK_MSG(err < 1e-6, std::format("post-crash {}", err));
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    for (int i = 0; i < 100 && proc->problem().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!proc->alive());
    CHECK(proc->problem().find("crashed") != std::string::npos);
    REQUIRE(s.run("RestartPlugin", {{"slotId", slot}}));
    CHECK(std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->alive());
}

TEST_CASE("vst3", "unload: removing the plugin ends its host process") {
    setup();
    int pid = 0;
    {
        Proj s;
        REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kGainCid)}}));
        const std::string slot = s.ctx->result["id"];
        s.render(2400);
        pid = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->hostPid();
        CHECK(isProcessAlive(pid));
    } // runtime + processor destroyed -> quit -> terminate/unload in the host
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        gone = !isProcessAlive(pid);
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(gone);
}

TEST_CASE("vst3", "safe mode skips third-party plugins and keeps their state; UI data never recreates the plugin") {
    setup();
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kGainCid)}, {"params", {{"0", 0.25}}}}));
    const std::string slot = s.ctx->result["id"];
    s.render(2400);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    const int pid = proc->hostPid();
    // UI-only data (favourite parameters) is undoable and does not restart the plugin
    REQUIRE(s.run("SetSlotUi", {{"slotId", slot}, {"key", "favorites"}, {"value", json::array({"0"})}}));
    REQUIRE(s.rt.rebuild(s.p));
    CHECK(std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->hostPid() == pid);
    CHECK(s.p.findSlot(slot)->state["ui"]["favorites"][0] == "0");
    REQUIRE(s.undo.undo());
    CHECK(!s.p.findSlot(slot)->state.contains("ui"));
    // editor commands (no display needed for the failure path: crashed/unknown slots are rejected)
    CHECK(!s.run("OpenPluginEditor", {{"slotId", "nope"}}));
    // SAFE MODE: a fresh runtime does not load the plugin, output = dry, state kept in the project
    s.rt.captureProcessorStates(s.p);
    const json saved = s.p.findSlot(slot)->state;
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime safe(engine);
    safe.setSafeMode(true);
    for (auto& a : s.p.assets) safe.addLoadedAsset(a.id, s.rt.asset(s.p, a.id));
    REQUIRE(safe.rebuild(s.p));
    CHECK(safe.processorForSlot(slot) == nullptr);
    bool warned = false;
    for (auto& w : safe.lastWarnings()) warned |= w.find("SAFE MODE") != std::string::npos;
    CHECK(warned);
    auto out = roytest::render(engine, 4800, 256);
    CHECK(rms(out[0], 1000, 4800) > 0.3); // unprocessed
    CHECK(s.p.findSlot(slot)->state == saved);
}
