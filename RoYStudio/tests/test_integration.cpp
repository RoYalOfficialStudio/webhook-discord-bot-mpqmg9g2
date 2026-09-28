// DEFINITION OF DONE - end-to-end workflow through the command system:
// NEW PROJECT -> AUDIO DEVICE -> CREATE TRACK -> RECORD VOCAL -> PLAYBACK -> EDIT VOCAL ->
// PITCH ANALYSIS -> OFF-KEY FILTER -> ADD BEAT -> DRUM PROGRAMMING -> 808 -> MIDI -> PIANO ROLL ->
// ARRANGEMENT -> MIXER -> EFFECTS (+ sandboxed CLAP plugin) -> AUTOMATION -> MIX -> MASTER ->
// EXPORT WAV -> SAVE -> CLOSE -> REOPEN -> identical project and identical render.
// Headless: the audio device is the miniaudio null backend and the "singer" is a synthetic
// voice (MOCK input, clearly labelled) fed through the real recording path.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "audio/DeviceManager.h"
#include "commands/Commands.h"
#include "io/AudioFile.h"
#include "plugins/Sandbox.h"
#include "project/ProjectIO.h"
#include "project/Session.h"
#include "record/Recorder.h"
#include "record/Takes.h"
#include "vocal/PitchDetector.h"

#include <thread>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

std::string channelByName(const Project& p, const std::string& name) {
    for (auto& c : p.channels)
        if (c.name == name) return c.id;
    return {};
}

struct Studio {
    AudioEngine engine;
    std::unique_ptr<ProjectRuntime> rt;
    Project p;
    std::unique_ptr<UndoManager> undo;
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    ProjectSession session;

    void attach(const fs::path& folder) {
        rt = std::make_unique<ProjectRuntime>(engine);
        rt->setProjectDirectory(folder);
        undo = std::make_unique<UndoManager>(p);
        ctx = std::make_unique<CommandContext>(CommandContext{p, *undo, rt.get(), folder});
        ctx->changed = [this](bool structural) {
            if (structural) rt->rebuild(p);
            else rt->syncParams(p);
        };
    }
    bool run(const std::string& id, const json& args = json::object()) {
        const bool ok = reg.execute(*ctx, id, args);
        if (!ok) std::fprintf(stderr, "command %s failed: %s\n", id.c_str(), ctx->error.c_str());
        if (ok) rt->rebuild(p);
        return ok;
    }
    std::string id() const { return ctx->result.value("id", ""); }
};

std::vector<std::vector<float>> readWav(const fs::path& f) {
    AudioData d;
    REQUIRE(readAudioFile(f, d));
    return d.channels;
}
} // namespace

TEST_CASE("integration", "definition of done: full production workflow survives save, close and reopen") {
    registerBuiltinProcessors();
    plugins::setHostExecutable(ROY_PLUGIN_HOST_EXE);
    registerPluginProcessors();
    const fs::path root = tempDir("dod");
    std::string err;

    // ---- NEW PROJECT ----------------------------------------------------------------
    Studio s;
    registerCoreCommands(s.reg);
    s.p = makeNewProject("DoD Song", SR, 90.0);
    REQUIRE(s.session.create(root, s.p, &err));
    const fs::path folder = s.session.folder();
    const fs::path projectFile = s.session.file();
    s.attach(folder);
    REQUIRE(s.run("SetKey", {{"key", "A Minor"}}));

    // ---- AUDIO DEVICE (null backend: no hardware in CI) ---------------------------
    {
        DeviceManager dm;
        REQUIRE(dm.initialise("null", &err));
        AudioDeviceConfig cfg;
        cfg.backend = "null";
        cfg.sampleRate = SR;
        cfg.bufferSize = 256;
        REQUIRE(dm.open(cfg, s.engine, &err));
        REQUIRE(dm.start(&err));
        const auto t0 = std::chrono::steady_clock::now();
        while (dm.callbackCount() < 5 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        dm.stop();
        CHECK(dm.callbackCount() >= 5);
        dm.close();
    }
    s.engine.prepare(SR, 256);

    // ---- CREATE TRACK ---------------------------------------------------------------
    const std::string vocalsBus = channelByName(s.p, "VOCALS"), drumsBus = channelByName(s.p, "DRUMS"), musicBus = channelByName(s.p, "MUSIC");
    REQUIRE(!vocalsBus.empty());
    REQUIRE(s.run("AddTrack", {{"type", "audio"}, {"name", "Lead Vocal"}, {"output", vocalsBus}, {"role", "vocal"}}));
    const std::string vocalTrack = s.id();
    const std::string vocalCh = s.ctx->result["channelId"];
    REQUIRE(s.run("ArmTrack", {{"trackId", vocalTrack}, {"armed", true}}));
    s.p.findTrack(vocalTrack)->inputLeft = 0;

    // ---- RECORD VOCAL (synthetic singer: A3 +30c, C4, E4 -40c, rests between) ------------
    VoiceSpec vs;
    vs.seconds = 6.0;
    vs.midi = [](double t) {
        if (t < 0.5) return 0.0;
        const double k = std::fmod(t - 0.5, 1.8);
        if (k > 1.4) return 0.0;
        const int n = static_cast<int>((t - 0.5) / 1.8) % 3;
        return n == 0 ? 57.3 : n == 1 ? 60.0 : 63.6;
    };
    const std::vector<float> voice = makeVoice(vs);
    Recorder rec;
    rec.prepare(SR, 256);
    rec.setOutputFolder(folder / "Audio");
    rec.setTracks(takes::recordConfig(s.p));
    s.engine.setInputListener(&rec);
    REQUIRE(s.rt->rebuild(s.p));
    REQUIRE(rec.startRecording(&err));
    s.engine.transport().seek(0);
    s.engine.transport().play();
    {
        std::vector<float> in(256), l(256), r(256);
        const float* ins[1] = {in.data()};
        float* outs[2] = {l.data(), r.data()};
        for (size_t done = 0; done < voice.size(); done += 256) {
            const size_t n = std::min<size_t>(256, voice.size() - done);
            std::fill(in.begin(), in.end(), 0.0f);
            std::copy(voice.begin() + static_cast<long>(done), voice.begin() + static_cast<long>(done + n), in.begin());
            s.engine.process(ins, 1, outs, 2, static_cast<int>(n));
        }
    }
    rec.stopRecording();
    s.engine.transport().stop();
    {
        std::vector<float> l(256), r(256);
        float* outs[2] = {l.data(), r.data()};
        s.engine.process(nullptr, 0, outs, 2, 256);
    }
    s.engine.setInputListener(nullptr);
    auto takesDone = rec.collectFinishedTakes();
    REQUIRE(takesDone.size() == 1);
    CHECK(rec.droppedSamples() == 0);
    REQUIRE(!takes::addRecordedTake(s.p, takesDone[0], folder).empty());
    auto flat = takes::flattenComp(s.p, vocalTrack);
    REQUIRE(flat.size() == 1);
    REQUIRE(s.run("ArmTrack", {{"trackId", vocalTrack}, {"armed", false}}));
    std::string vocalClip = s.p.findTrack(vocalTrack)->audioClips.at(0).id;

    // ---- PLAYBACK --------------------------------------------------------------------
    REQUIRE(s.rt->rebuild(s.p));
    {
        auto out = render(s.engine, static_cast<int64_t>(3.0 * SR));
        CHECK(rms(out[0], 0, static_cast<size_t>(0.4 * SR)) < 1e-4);
        CHECK(rms(out[0], static_cast<size_t>(0.8 * SR), static_cast<size_t>(1.6 * SR)) > 0.01);
    }

    // ---- EDIT VOCAL --------------------------------------------------------------------
    REQUIRE(s.run("SetFades", {{"clipId", vocalClip}, {"fadeInBeats", 0.1}, {"fadeOutBeats", 0.25}}));
    REQUIRE(s.run("SetClipGain", {{"clipId", vocalClip}, {"gainDb", 1.5}}));

    // ---- PITCH ANALYSIS ------------------------------------------------------------------
    REQUIRE(s.run("PitchAnalysis", {{"clipId", vocalClip}}));
    const double inTuneBefore = s.ctx->result["inTuneRatio"];
    CHECK(s.ctx->result["notes"].size() >= 3);
    CHECK(!s.ctx->result["issues"].empty());

    // ---- OFF-KEY FILTER (Pitch Guardian, lock to A minor) ------------------------------------
    const std::string assetBefore = s.p.findAudioClip(vocalClip)->assetId;
    REQUIRE(s.run("PitchGuardian", {{"clipId", vocalClip}, {"mode", "lock"}, {"offKeyFilter", true}, {"speedMs", 10.0}}));
    CHECK(s.ctx->result["corrected"].get<int>() >= 2);
    CHECK(s.p.findAudioClip(vocalClip)->assetId != assetBefore); // new derived file, original kept
    CHECK(fs::exists(folder / s.p.findAsset(assetBefore)->path));
    REQUIRE(s.run("PitchAnalysis", {{"clipId", vocalClip}}));
    CHECK(s.ctx->result["inTuneRatio"].get<double>() > inTuneBefore);

    // ---- ADD BEAT + DRUM PROGRAMMING ----------------------------------------------------------
    REQUIRE(s.run("AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"output", drumsBus}}));
    const std::string beatTrack = s.id();
    REQUIRE(s.run("AddPattern", {{"name", "Main Beat"}, {"steps", 16}}));
    const std::string pattern = s.id();
    REQUIRE(s.run("SetRowPattern", {{"patternId", pattern}, {"voice", "kick"}, {"text", "X.....x...x....."}}));
    REQUIRE(s.run("SetRowPattern", {{"patternId", pattern}, {"voice", "snare"}, {"text", "....X.......X..."}}));
    REQUIRE(s.run("SetRowPattern", {{"patternId", pattern}, {"voice", "closed_hat"}, {"text", "x.o.x.o.x.o.x.o."}}));
    REQUIRE(s.run("SetStep", {{"patternId", pattern}, {"voice", "open_hat"}, {"step", 14}, {"on", true}, {"velocity", 0.6}}));
    REQUIRE(s.run("SetPatternSwing", {{"patternId", pattern}, {"swing", 0.2}}));
    REQUIRE(s.run("AddPatternClip", {{"trackId", beatTrack}, {"patternId", pattern}, {"startBeat", 0.0}, {"lengthBeats", 16.0}}));

    // ---- 808 --------------------------------------------------------------------------------------
    REQUIRE(s.run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"output", drumsBus}, {"instrument", "roy.808"}}));
    const std::string bassTrack = s.id();
    REQUIRE(s.run("AddMidiClip", {{"trackId", bassTrack}, {"startBeat", 0.0}, {"lengthBeats", 16.0}}));
    const std::string bassClip = s.id();
    REQUIRE(s.run("AddNote", {{"clipId", bassClip}, {"pitch", 33}, {"startBeat", 0.0}, {"lengthBeats", 1.5}}));
    REQUIRE(s.run("AddNote", {{"clipId", bassClip}, {"pitch", 36}, {"startBeat", 2.5}, {"lengthBeats", 1.0}, {"slide", true}}));
    REQUIRE(s.run("AddNote", {{"clipId", bassClip}, {"pitch", 31}, {"startBeat", 4.0}, {"lengthBeats", 2.0}}));

    // ---- MIDI + PIANO ROLL (with wrong-note blocker) --------------------------------------------
    REQUIRE(s.run("AddTrack", {{"type", "midi"}, {"name", "Keys"}, {"output", musicBus}}));
    const std::string keysTrack = s.id();
    const std::string keysCh = s.ctx->result["channelId"];
    REQUIRE(s.run("AddMidiClip", {{"trackId", keysTrack}, {"startBeat", 0.0}, {"lengthBeats", 8.0}}));
    const std::string keysClip = s.id();
    for (int pitch : {57, 60, 64}) REQUIRE(s.run("AddNote", {{"clipId", keysClip}, {"pitch", pitch}, {"startBeat", 0.03}, {"lengthBeats", 3.9}}));
    for (int pitch : {53, 57, 60}) REQUIRE(s.run("AddNote", {{"clipId", keysClip}, {"pitch", pitch}, {"startBeat", 4.02}, {"lengthBeats", 3.9}}));
    CHECK(!s.reg.execute(*s.ctx, "AddNote", {{"clipId", keysClip}, {"pitch", 61}, {"wrongNoteMode", "block"}})); // C# not in A minor
    REQUIRE(s.run("QuantizeNotes", {{"clipId", keysClip}, {"grid", 0.25}, {"strength", 1.0}}));
    CHECK_NEAR(s.p.findMidiClip(keysClip)->notes[0].startBeat, 0.0, 1e-9);

    // ---- ARRANGEMENT ---------------------------------------------------------------------------
    REQUIRE(s.run("AddSection", {{"name", "Intro"}, {"type", "intro"}, {"startBeat", 0.0}, {"endBeat", 4.0}}));
    REQUIRE(s.run("AddSection", {{"name", "Verse"}, {"type", "verse"}, {"startBeat", 4.0}, {"endBeat", 16.0}}));
    REQUIRE(s.run("DuplicateClip", {{"clipId", keysClip}}));
    REQUIRE(s.run("AddMarker", {{"name", "Drop"}, {"beat", 8.0}}));

    // ---- MIXER ---------------------------------------------------------------------------------
    REQUIRE(s.run("SetChannelGain", {{"channelId", vocalCh}, {"gainDb", -1.0}}));
    REQUIRE(s.run("SetChannelPan", {{"channelId", keysCh}, {"pan", -0.2}}));
    REQUIRE(s.run("AddBus", {{"name", "Vocal Reverb"}}));
    const std::string reverbBus = s.id();
    REQUIRE(s.run("AddSend", {{"channelId", vocalCh}, {"target", reverbBus}, {"levelDb", -12.0}}));

    // ---- EFFECTS (built-in + sandboxed CLAP plugin) -----------------------------------------------
    REQUIRE(s.run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.eq"}, {"name", "EQ"}}));
    REQUIRE(s.run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.compressor"}, {"name", "Comp"}}));
    REQUIRE(s.run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.deesser"}, {"name", "De-Ess"}}));
    REQUIRE(s.run("AddInsert", {{"channelId", reverbBus}, {"typeId", "roy.reverb"}, {"name", "Reverb"}}));
    const std::string clapType = plugins::makeClapTypeId((fs::path(ROY_TEST_PLUGIN_DIR) / "roy_test_gain.clap").string(), "com.roystudio.test.gain");
    REQUIRE(s.run("AddInsert", {{"channelId", keysCh}, {"typeId", clapType}, {"name", "CLAP Gain"}}));
    const std::string clapSlot = s.id();
    REQUIRE(s.run("SetParam", {{"slotId", clapSlot}, {"paramId", "0"}, {"value", 0.7}}));
    auto clapProc = std::dynamic_pointer_cast<SandboxedPluginProcessor>(s.rt->processorForSlot(clapSlot));
    REQUIRE(clapProc != nullptr);
    CHECK(clapProc->alive());

    // ---- AUTOMATION ------------------------------------------------------------------------------
    REQUIRE(s.run("CreateAutomation", {{"channelId", keysCh}, {"paramId", "gain"}, {"points", {{0.0, 0.6}, {8.0, 0.9}, {16.0, 0.8}}}}));

    // ---- MIX ------------------------------------------------------------------------------------
    REQUIRE(s.run("AnalyzeMix", {{"startBeat", 0.0}, {"endBeat", 16.0}}));
    CHECK(s.ctx->result.contains("master"));

    // ---- MASTER ----------------------------------------------------------------------------------
    REQUIRE(s.run("CreateMasterChain", {{"preset", "streaming"}}));

    // ---- EXPORT WAV --------------------------------------------------------------------------------
    REQUIRE(s.run("Export", {{"format", "wav"}, {"bitDepth", 24}, {"dither", "none"}, {"name", "dod_mix"}, {"folder", (folder / "Exports").string()}}));
    REQUIRE(s.ctx->result["files"].size() >= 1);
    const fs::path wav1 = s.ctx->result["files"][0]["path"].get<std::string>();
    CHECK(fs::exists(wav1));
    const double tp = s.ctx->result["files"][0]["truePeakDb"];
    CHECK_MSG(tp <= -0.8, std::format("true peak {} dBTP", tp));
    auto mix1 = readWav(wav1);
    REQUIRE(mix1.size() == 2);
    CHECK(mix1[0].size() >= static_cast<size_t>(16 * 60.0 / 90.0 * SR));
    CHECK(allFinite(mix1[0]));
    CHECK(rms(mix1[0]) > 0.01);

    // a second export in the same session is bit-identical (clean processor state per render)
    REQUIRE(s.run("Export", {{"format", "wav"}, {"bitDepth", 24}, {"dither", "none"}, {"name", "dod_mix_again"}, {"folder", (folder / "Exports").string()}}));
    {
        auto again = readWav(s.ctx->result["files"][0]["path"].get<std::string>());
        REQUIRE(again[0].size() == mix1[0].size());
        CHECK(again[0] == mix1[0]);
        CHECK(again[1] == mix1[1]);
    }

    // ---- UNDO / REDO ------------------------------------------------------------------------------
    const std::string beforeUndo = projectToJson(s.p).dump();
    REQUIRE(s.undo->undo()); // master chain
    CHECK(projectToJson(s.p).dump() != beforeUndo);
    REQUIRE(s.undo->redo());
    CHECK(projectToJson(s.p).dump() == beforeUndo);

    // ---- SAVE + AUTOSAVE -------------------------------------------------------------------------------
    s.rt->captureProcessorStates(s.p);
    CHECK(s.p.findSlot(clapSlot)->state.contains("clap"));
    REQUIRE(s.session.autosave(s.p, true));
    REQUIRE(s.session.save(s.p, &err));
    const std::string saved = projectToJson(s.p).dump();

    // ---- CLOSE ---------------------------------------------------------------------------------------
    s.session.close();
    s.rt.reset(); // plugin host processes end here
    CHECK(!fs::exists(folder / ".roy_session.lock"));

    // ---- REOPEN ----------------------------------------------------------------------------------------
    Studio r;
    registerCoreCommands(r.reg);
    REQUIRE(r.session.open(projectFile, r.p, OpenMode::Normal, &err));
    r.engine.prepare(SR, 256);
    r.attach(folder);
    {
        json a = json::parse(saved), b = projectToJson(r.p);
        a.erase("modifiedAt"); // stamped by save()
        b.erase("modifiedAt");
        CHECK(a == b);
    }
    REQUIRE(r.rt->rebuild(r.p));
    CHECK(r.rt->lastWarnings().empty());
    auto reClap = std::dynamic_pointer_cast<SandboxedPluginProcessor>(r.rt->processorForSlot(clapSlot));
    REQUIRE(reClap != nullptr);
    CHECK_NEAR(reClap->getParam(0), 0.7, 1e-6);
    REQUIRE(r.run("Export", {{"format", "wav"}, {"bitDepth", 24}, {"dither", "none"}, {"name", "dod_mix_reopened"}, {"folder", (folder / "Exports").string()}}));
    auto mix2 = readWav(r.ctx->result["files"][0]["path"].get<std::string>());
    REQUIRE(mix2.size() == 2);
    REQUIRE(mix2[0].size() == mix1[0].size());
    double maxDiff = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < mix1[static_cast<size_t>(c)].size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(mix1[static_cast<size_t>(c)][i] - mix2[static_cast<size_t>(c)][i])));
    CHECK_MSG(maxDiff == 0.0, std::format("reopened render differs by {}", maxDiff)); // bit-identical
    r.session.close();
}
