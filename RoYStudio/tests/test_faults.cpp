// FAULT INJECTION: RoY must not lose user data when things go wrong.
// Plugin crash / plugin hang, missing audio file, corrupt project, invalid plugin state,
// audio device loss, export failure and a full disk while saving, exporting and recording.
// Disk faults use files::fault (TEST-ONLY hook), device loss uses
// DeviceManager::simulateDeviceLoss (stops the device behind RoY's back, as a real loss does).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "audio/DeviceManager.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "core/Process.h"
#include "export/Exporter.h"
#include "io/AudioFile.h"
#include "plugins/Sandbox.h"
#include "project/ProjectIO.h"
#include "record/Recorder.h"
#include "record/Takes.h"

#include <fstream>
#include <random>
#include <thread>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;
const fs::path kPluginDir = ROY_TEST_PLUGIN_DIR;
const fs::path kBundle = kPluginDir / "RoYTest.vst3";
const std::string kHangCid = "524F590448414E47524F433100000006";

struct FaultGuard { // never leave a disk fault active for the next test
    ~FaultGuard() { files::fault::clear(); }
};

void setup() {
    plugins::setHostExecutable(ROY_PLUGIN_HOST_EXE);
    registerBuiltinProcessors();
    registerPluginProcessors();
}

bool anyFileContaining(const fs::path& dir, const std::string& fragment) {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.path().filename().string().find(fragment) != std::string::npos) return true;
    return false;
}

struct Proj {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("faults", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string trackId, channelId;
    Proj() {
        engine.prepare(SR, 256);
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        const std::string asset = addMemoryAsset(p, rt, makeSine(SR, 220.0, 6.0, 0.5f));
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

TEST_CASE("faults", "disk full while saving: the previous project file stays intact, the project stays in memory") {
    FaultGuard guard;
    const fs::path dir = tempDir("fault_save");
    const fs::path file = dir / "song.roy";
    Project p = makeNewProject("song", SR, 120.0);
    addTrack(p, TrackType::Audio, "Vox");
    REQUIRE(saveProject(p, file).ok);
    const std::string before = *files::readAll(file);

    addTrack(p, TrackType::Audio, "Adlib");
    files::fault::setDiskFull("song.roy", 100); // the disk fills up after 100 bytes
    auto r = saveProject(p, file);
    CHECK(!r.ok);
    CHECK(r.error.find("untouched") != std::string::npos);
    CHECK(*files::readAll(file) == before);             // old file byte-identical
    CHECK(!anyFileContaining(dir, ".tmp"));             // no half-written temp file left behind
    CHECK(p.tracks.size() == 2);                        // nothing lost in memory
    Project reread;
    REQUIRE(loadProject(file, reread).ok);
    CHECK(reread.tracks.size() == 1);

    files::fault::clear(); // space freed -> saving works again
    REQUIRE(saveProject(p, file).ok);
    REQUIRE(loadProject(file, reread).ok);
    CHECK(reread.tracks.size() == 2);
}

TEST_CASE("faults", "export failure: full disk and a folder that is a file fail cleanly, no broken files") {
    FaultGuard guard;
    Proj s;
    const fs::path dir = tempDir("fault_export");
    for (auto fmt : {exporting::Format::Wav, exporting::Format::Flac, exporting::Format::Mp3}) {
        if (fmt == exporting::Format::Mp3 && !mp3::available()) {
            CHECK_MSG(false, "MP3 encoder library not found");
            continue;
        }
        exporting::ExportOptions o;
        o.format = fmt;
        o.folder = dir;
        const std::string base = std::string("full_disk_") + (exporting::extensionFor(fmt) + 1);
        o.baseName = base;
        o.range = exporting::Range::Selection;
        o.startBeat = 0;
        o.endBeat = 4;
        files::fault::setDiskFull(base, 4096);
        auto r = exporting::exportProject(s.engine, s.rt, s.p, o);
        CHECK_MSG(!r.ok, exporting::extensionFor(fmt));
        CHECK(!r.error.empty());
        CHECK_MSG(!anyFileContaining(dir, base), std::format("leftover file for {}", exporting::extensionFor(fmt)));
        files::fault::clear();
        auto ok = exporting::exportProject(s.engine, s.rt, s.p, o); // space freed -> works
        CHECK_MSG(ok.ok, ok.error);
    }
    // the export folder is actually a file
    const fs::path notAFolder = dir / "i_am_a_file";
    { std::ofstream(notAFolder) << "x"; }
    exporting::ExportOptions o;
    o.folder = notAFolder;
    o.baseName = "mix";
    o.range = exporting::Range::Selection;
    o.endBeat = 2;
    auto r = exporting::exportProject(s.engine, s.rt, s.p, o);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(*files::readAll(notAFolder) == "x");
    // the engine keeps playing normally after failed exports
    auto out = s.render(4800);
    CHECK(peak(out[0]) > 0.1f);
}

TEST_CASE("faults", "disk full while recording: the take keeps what was written, the performance is recoverable") {
    FaultGuard guard;
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Recorder rec;
    const fs::path dir = tempDir("fault_record");
    Project p = makeNewProject("rec", SR, 120.0);
    const std::string trackId = addTrack(p, TrackType::Audio, "Lead Vox").id;
    p.findTrack(trackId)->armed = true;
    rec.prepare(SR, 256);
    rec.setOutputFolder(dir / "Audio");
    rec.setFormat(SampleFormat::Pcm24);
    rec.setTracks(takes::recordConfig(p));
    engine.setInputListener(&rec);
    REQUIRE(rt.rebuild(p));
    auto signal = [](int64_t i) { return 0.5f * static_cast<float>(std::sin(kTwoPi * 330.0 * i / SR)); };

    // mono 24-bit = 3 bytes per frame: the disk fills up after 24000 frames (+1 stray byte)
    files::fault::setDiskFull("_Take", 3 * 24000 + 1);
    REQUIRE(rec.startRecording());
    engine.transport().play();
    std::vector<float> in(256), l(256), r(256);
    const float* ins[1] = {in.data()};
    float* outs[2] = {l.data(), r.data()};
    int64_t fed = 0;
    for (int b = 0; b < 48000 / 256 + 1; ++b) {
        for (int i = 0; i < 256; ++i) in[static_cast<size_t>(i)] = signal(fed + i);
        engine.process(ins, 1, outs, 2, 256);
        fed += 256;
    }
    rec.flush();
    rec.stopRecording();
    engine.transport().stop();
    engine.process(ins, 1, outs, 2, 256);
    auto taken = rec.collectFinishedTakes();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].diskError);
    CHECK(taken[0].frames == 24000);
    CHECK(rec.diskErrors() >= 1);
    CHECK(rec.lastDiskError().find("Recover") != std::string::npos);
    AudioData d;
    REQUIRE(readAudioFile(taken[0].path, d)); // a valid WAV with every frame that fit on the disk
    CHECK(d.numFrames == 24000);
    double err = 0;
    for (int64_t i = 0; i < d.numFrames; ++i) err = std::max(err, std::fabs(double(d.channels[0][static_cast<size_t>(i)]) - signal(i)));
    CHECK(err < 1e-5);

    files::fault::clear(); // after freeing space the full performance comes back from the never-lose buffer
    std::string e;
    auto rescued = rec.recoverLastPerformance(trackId, false, &e);
    REQUIRE_MSG_OK(rescued.has_value(), e);
    CHECK(rescued->frames >= 48000);
    engine.setInputListener(nullptr);
}

TEST_CASE("faults", "missing audio file: warning, silence for that clip, the rest plays; restored file plays again") {
    FaultGuard guard;
    AudioEngine engine;
    engine.prepare(SR, 256);
    const fs::path dir = tempDir("fault_missing");
    ProjectRuntime rt(engine);
    rt.setProjectDirectory(dir);
    Project p = makeNewProject("missing", SR, 120.0);
    fs::create_directories(dir / "Audio");
    auto sine = makeSine(SR, 440.0, 4.0, 0.5f);
    REQUIRE(writeWavFile(dir / "Audio" / "vox.wav", sine->channels, SR, SampleFormat::Float32));
    AudioAsset a;
    a.id = "vox";
    a.path = "Audio/vox.wav";
    a.originalName = "vox.wav";
    a.sampleRate = SR;
    a.channels = 2;
    a.frames = sine->numFrames;
    p.assets.push_back(a);
    Track& t = addTrack(p, TrackType::Audio, "Vox");
    addClip(t, "vox", 0, 4.0);
    const std::string other = addMemoryAsset(p, rt, makeSine(SR, 110.0, 4.0, 0.25f));
    addClip(addTrack(p, TrackType::Audio, "Bass"), other, 0, 4.0);

    fs::rename(dir / "Audio" / "vox.wav", dir / "vox_moved.wav"); // user moved the file away
    REQUIRE(rt.rebuild(p));                                      // the project still opens
    bool warned = false;
    for (auto& w : rt.lastWarnings()) warned |= w.find("vox.wav") != std::string::npos;
    CHECK(warned);
    auto out = render(engine, 24000);
    CHECK(allFinite(out[0]));
    CHECK_NEAR(peak(out[0], 4800), 0.25f * 0.7f, 0.1f); // only the bass (pan law) - no crash, no garbage
    CHECK(p.assets.size() == 2);                         // the missing asset is kept, not deleted

    fs::rename(dir / "vox_moved.wav", dir / "Audio" / "vox.wav"); // file restored -> plays again
    REQUIRE(rt.rebuild(p));
    engine.transport().seek(0);
    auto out2 = render(engine, 24000);
    CHECK(peak(out2[0], 4800) > peak(out[0], 4800) + 0.1f);
}

TEST_CASE("faults", "corrupt project files never crash: truncated, bit-flipped and type-confused") {
    setup();
    FaultGuard guard;
    Project p = makeNewProject("corrupt", SR, 120.0);
    for (int i = 0; i < 4; ++i) addTrack(p, i % 2 ? TrackType::Midi : TrackType::Audio, std::format("T{}", i));
    const std::string good = serializeProject(p);
    int loaded = 0, rejected = 0;
    auto tryText = [&](const std::string& text) {
        Project q;
        std::string err;
        if (deserializeProject(text, q, &err)) {
            ++loaded;
            AudioEngine engine; // whatever loads must also compile and render
            engine.prepare(SR, 256);
            ProjectRuntime rt(engine);
            rt.rebuild(q);
            auto out = render(engine, 1024);
            CHECK(allFinite(out[0]));
        } else {
            ++rejected;
            CHECK(!err.empty());
        }
    };
    for (size_t cut = 0; cut < good.size(); cut += std::max<size_t>(1, good.size() / 60)) tryText(good.substr(0, cut));
    std::mt19937 rng(1234);
    for (int k = 0; k < 300; ++k) {
        std::string t = good;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) t[rng() % t.size()] = static_cast<char>(rng() % 256);
        tryText(t);
    }
    // structurally valid JSON with wrong types / absurd values
    json j = json::parse(good);
    for (const char* key : {"tracks", "channels", "assets", "patterns", "tempo", "markers"}) {
        for (json bad : {json(5), json("x"), json(nullptr), json::array({1, 2}), json::object()}) {
            json k = j;
            k[key] = bad;
            tryText(k.dump());
        }
    }
    json huge = j;
    huge["tempo"] = json{{"bpm", 1e308}};
    tryText(huge.dump());
    CHECK(rejected > 0);
    CHECK(loaded > 0);
    std::printf("%s\n", std::format("corrupt variants: {} loaded (repaired), {} rejected with a message", loaded, rejected).c_str());
}

TEST_CASE("faults", "invalid plugin state: plugin runs with defaults, the rejected state is kept in the project") {
    setup();
    FaultGuard guard;
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeClapTypeId((kPluginDir / "roy_test_gain.clap").string(), "com.roystudio.test.gain")}}));
    const std::string slot = s.ctx->result["id"];
    s.render(2400);
    // a damaged plugin chunk (e.g. a newer plugin version wrote it)
    const std::string damaged = "!!! not base64 !!!";
    s.p.findSlot(slot)->state["plugin"] = {{"format", "clap"}, {"state", damaged}, {"pluginName", "RoY Test Gain"}};
    REQUIRE(s.rt.rebuild(s.p));
    bool warned = false;
    for (auto& w : s.rt.lastWarnings()) warned |= w.find("rejected its saved state") != std::string::npos;
    CHECK(warned);
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    CHECK(proc->alive()); // still usable
    auto out = roytest::render(s.engine, 4800, 256);
    CHECK(allFinite(out[0]));
    // saving keeps the rejected chunk next to the new state: nothing is thrown away
    s.rt.captureProcessorStates(s.p);
    const json saved = s.p.findSlot(slot)->state;
    CHECK(saved["plugin"].value("rejectedState", "") == damaged);
    CHECK(saved["plugin"].value("state", "") != damaged);
    // ... and survives a save / reload round trip
    const fs::path dir = tempDir("fault_pluginstate");
    REQUIRE(saveProject(s.p, dir / "p.roy").ok);
    Project q;
    REQUIRE(loadProject(dir / "p.roy", q).ok);
    CHECK(q.findSlot(slot)->state["plugin"].value("rejectedState", "") == damaged);
}

TEST_CASE("faults", "plugin hang during playback: detected, host terminated, pass-through, restart") {
    setup();
    plugins::takeCrashEvents();
    Proj ref;
    auto dry = ref.render(48000);
    Proj s;
    REQUIRE(s.run("AddInsert", {{"channelId", s.channelId}, {"typeId", plugins::makeVst3TypeId(kBundle.string(), kHangCid)}}));
    const std::string slot = s.ctx->result["id"];
    auto out = s.render(48000); // the plugin stops answering after 20 blocks
    double err = 0;
    for (size_t i = 24000; i < 48000; ++i) err = std::max(err, static_cast<double>(std::fabs(out[0][i] - dry[0][i])));
    CHECK_MSG(err < 1e-6, std::format("post-hang {}", err));
    auto proc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot));
    REQUIRE(proc != nullptr);
    const int pid = proc->hostPid();
    for (int i = 0; i < 300 && proc->problem().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!proc->alive());
    CHECK(proc->problem().find("hung") != std::string::npos);
    bool gone = false; // the watchdog terminates the frozen host
    for (int i = 0; i < 500 && !gone; ++i) {
        gone = !isProcessAlive(pid);
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(gone);
    bool reported = false;
    for (auto& e : plugins::takeCrashEvents()) reported |= e.reason.find("hung") != std::string::npos;
    CHECK(reported);
    REQUIRE(s.run("RestartPlugin", {{"slotId", slot}}));
    CHECK(std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt.processorForSlot(slot))->alive());
}

TEST_CASE("faults", "audio device loss: detected once, RoY keeps running, reconnects when the device returns") {
    AudioEngine engine;
    DeviceManager dm;
    std::string err;
    REQUIRE_MSG_OK(dm.initialise("null", &err), err);
    AudioDeviceConfig cfg;
    cfg.backend = "null";
    cfg.bufferSize = 256;
    REQUIRE_MSG_OK(dm.open(cfg, engine, &err), err);
    REQUIRE_MSG_OK(dm.start(&err), err);
    double t = 0;
    CHECK(dm.poll(t) == DeviceManager::Health::Ok);
    for (int i = 0; i < 200 && dm.callbackCount() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(dm.callbackCount() >= 3);

    dm.simulateDeviceAbsent(true);
    dm.simulateDeviceLoss();
    CHECK(dm.poll(t += 0.1) == DeviceManager::Health::Lost);
    CHECK(dm.deviceLost());
    CHECK(!dm.lostReason().empty());
    CHECK(!dm.isRunning());
    CHECK(dm.poll(t += 0.1) == DeviceManager::Health::StillLost); // reported only once
    CHECK(dm.poll(t += 2.0) == DeviceManager::Health::StillLost); // device still gone
    CHECK(dm.reconnectAttempts() >= 1);
    // the engine still renders offline while the device is gone (export keeps working)
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    engine.process(nullptr, 0, outs, 2, 256);

    dm.simulateDeviceAbsent(false); // plugged back in
    DeviceManager::Health h = DeviceManager::Health::StillLost;
    for (int i = 0; i < 10 && h != DeviceManager::Health::Reconnected; ++i) h = dm.poll(t += 6.0);
    CHECK(h == DeviceManager::Health::Reconnected);
    CHECK(!dm.deviceLost());
    CHECK(dm.isRunning());
    const uint64_t c0 = dm.callbackCount();
    for (int i = 0; i < 200 && dm.callbackCount() < c0 + 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(dm.callbackCount() >= c0 + 3);
    CHECK(dm.poll(t += 0.1) == DeviceManager::Health::Ok);
    dm.close();
}

TEST_CASE("faults", "stalled audio callbacks are detected as a lost device") {
    AudioEngine engine;
    DeviceManager dm;
    std::string err;
    REQUIRE(dm.initialise("null", &err));
    AudioDeviceConfig cfg;
    cfg.backend = "null";
    REQUIRE(dm.open(cfg, engine, &err));
    REQUIRE(dm.start(&err));
    for (int i = 0; i < 200 && dm.callbackCount() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(dm.poll(0.0) == DeviceManager::Health::Ok);
    dm.simulateCallbackStall(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let an in-flight callback finish
    CHECK(dm.poll(1.0) == DeviceManager::Health::Ok);
    CHECK(dm.poll(1.0 + DeviceManager::kStallSeconds + 0.5) == DeviceManager::Health::Lost);
    CHECK(dm.lostReason().find("stalled") != std::string::npos);
    dm.close();
}
