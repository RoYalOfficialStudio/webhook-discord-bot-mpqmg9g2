// GMB 06 - Vocal Lab commands on a real project folder: non-destructive,
// undoable, verified through the audio engine.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "commands/Commands.h"
#include "io/AudioFile.h"
#include "project/ProjectIO.h"
#include "vocal/PitchDetector.h"

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

struct VocalProject {
    fs::path dir;
    Project p;
    AudioEngine engine;
    std::unique_ptr<ProjectRuntime> rt;
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string trackId, clipId, assetId;

    VocalProject(const std::string& name, const std::vector<float>& vocal) {
        dir = tempDir(name);
        fs::create_directories(dir / "Audio");
        p = makeNewProject(name, SR, 120.0);
        p.key = *parseKey("C Major");
        REQUIRE(writeWavFile(dir / "Audio" / "vox.wav", {vocal}, SR, SampleFormat::Float32));
        AudioAsset a;
        a.id = files::newId();
        a.path = "Audio/vox.wav";
        a.originalName = "vox.wav";
        a.sampleRate = SR;
        a.channels = 1;
        a.frames = static_cast<int64_t>(vocal.size());
        p.assets.push_back(a);
        assetId = a.id;
        trackId = addTrack(p, TrackType::Audio, "Lead Vocal").id;
        AudioClip c;
        c.id = files::newId();
        c.assetId = a.id;
        c.startBeat = 0;
        c.lengthBeats = p.tempo.secondsToBeat(static_cast<double>(vocal.size()) / SR);
        p.findTrack(trackId)->audioClips.push_back(c);
        clipId = c.id;
        engine.prepare(SR, 512);
        rt = std::make_unique<ProjectRuntime>(engine);
        rt->setProjectDirectory(dir);
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, rt.get(), dir});
    }
    bool run(const std::string& id, const json& args) { return reg.execute(*ctx, id, args); }
    std::vector<float> renderMono(double seconds) {
        REQUIRE(rt->rebuild(p));
        return render(engine, static_cast<int64_t>(seconds * SR))[0];
    }
};

double median(const vocal::PitchTrack& t, double a, double b) {
    std::vector<double> v;
    for (auto& f : t.frames)
        if (f.voiced && f.time >= a && f.time < b) v.push_back(f.midi);
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

TEST_CASE("gmb06", "Pitch Guardian command is non-destructive and undoable") {
    VoiceSpec v;
    v.seconds = 1.0;
    v.midi = [](double) { return 64.4; }; // E + 40 cents
    VocalProject vp("vocal_cmd_guardian", makeVoice(v));
    const std::string origHash = files::sha256File(vp.dir / "Audio" / "vox.wav");
    REQUIRE(vp.run("PitchGuardian", {{"clipId", vp.clipId}, {"mode", "assist"}, {"speedMs", 5.0}}));
    CHECK(vp.ctx->result["corrected"].get<int>() >= 1);
    const std::string newAsset = vp.p.findAudioClip(vp.clipId)->assetId;
    CHECK(newAsset != vp.assetId);
    CHECK(vp.p.findAsset(newAsset)->kind == "derived");
    CHECK(fs::exists(vp.ctx->result["file"].get<std::string>()));
    CHECK(files::sha256File(vp.dir / "Audio" / "vox.wav") == origHash); // original untouched
    CHECK(vp.p.vocalSettings[vp.trackId]["pitchGuardian"]["mode"] == "assist");
    auto out = vp.renderMono(1.0);
    auto t = vocal::detectPitch(out.data(), static_cast<int64_t>(out.size()), SR);
    CHECK_NEAR(median(t, 0.2, 0.8), 64.0, 0.06);
    REQUIRE(vp.undo.undo());
    CHECK(vp.p.findAudioClip(vp.clipId)->assetId == vp.assetId);
    auto orig = vp.renderMono(1.0);
    auto t2 = vocal::detectPitch(orig.data(), static_cast<int64_t>(orig.size()), SR);
    CHECK_NEAR(median(t2, 0.2, 0.8), 64.4, 0.05);
    // WARN mode analyses only: no new file, no change
    const size_t assets = vp.p.assets.size();
    REQUIRE(vp.run("PitchGuardian", {{"clipId", vp.clipId}, {"mode", "warn"}}));
    CHECK(vp.p.assets.size() == assets);
    CHECK(!vp.ctx->result["warnings"].empty());
}

TEST_CASE("gmb06", "Vocal Doctor command returns executable fixes") {
    VoiceSpec v;
    v.seconds = 1.5;
    v.midi = [](double t) { return t < 1.0 ? 60.0 : 0.0; };
    auto x = makeVoice(v);
    for (size_t i = 0; i < x.size(); ++i) x[i] += 0.1f * static_cast<float>(std::sin(kTwoPi * 40.0 * static_cast<double>(i) / SR));
    VocalProject vp("vocal_cmd_doctor", x);
    REQUIRE(vp.run("VocalDoctor", {{"clipId", vp.clipId}}));
    json rep = vp.ctx->result;
    json rumbleFix;
    for (auto& i : rep["issues"])
        if (i["id"] == "rumble") rumbleFix = i["fixes"][0];
    REQUIRE(!rumbleFix.is_null());
    CHECK(rumbleFix["args"]["channelId"] == vp.p.findTrack(vp.trackId)->channelId);
    CHECK(rumbleFix["command"] == "AddInsert");
}

TEST_CASE("gmb06", "breath reduction writes undoable automation") {
    VoiceSpec v;
    v.seconds = 2.0;
    VocalProject vp("vocal_cmd_breath", makeVoice(v));
    REQUIRE(vp.run("BreathReduce", {{"clipId", vp.clipId}, {"regions", {{{"start", 0.5}, {"end", 0.9}}}}, {"reductionDb", -10.0}}));
    REQUIRE(vp.p.automation.size() == 1);
    auto out = vp.renderMono(2.0);
    CHECK(rms(out, static_cast<size_t>(0.6 * SR), static_cast<size_t>(0.8 * SR)) <
          rms(out, static_cast<size_t>(1.2 * SR), static_cast<size_t>(1.4 * SR)) * 0.4);
    REQUIRE(vp.undo.undo());
    CHECK(vp.p.automation.empty());
}

TEST_CASE("gmb06", "Double Magnet command aligns a double track") {
    auto syl = [](const std::vector<double>& times) {
        VoiceSpec v;
        v.seconds = 2.2;
        v.midi = [times](double t) {
            for (double s : times)
                if (t >= s && t < s + 0.14) return 57.0;
            return 0.0;
        };
        auto x = makeVoice(v);
        for (double s : times)
            for (size_t i = 0; i < static_cast<size_t>(0.14 * SR); ++i) {
                const double t = static_cast<double>(i) / SR;
                x[static_cast<size_t>(s * SR) + i] *= static_cast<float>(std::min(1.0, t / 0.004) * std::min(1.0, (0.14 - t) / 0.02));
            }
        return x;
    };
    VocalProject vp("vocal_cmd_magnet", syl({0.2, 0.6, 1.0, 1.4, 1.8}));
    std::vector<float> dbl = syl({0.25, 0.63, 0.96, 1.46, 1.83});
    REQUIRE(writeWavFile(vp.dir / "Audio" / "dbl.wav", {dbl}, SR, SampleFormat::Float32));
    AudioAsset a;
    a.id = files::newId();
    a.path = "Audio/dbl.wav";
    a.originalName = "dbl.wav";
    a.frames = static_cast<int64_t>(dbl.size());
    vp.p.assets.push_back(a);
    const std::string dt = addTrack(vp.p, TrackType::Audio, "Double").id;
    AudioClip c;
    c.id = files::newId();
    c.assetId = a.id;
    c.lengthBeats = vp.p.findAudioClip(vp.clipId)->lengthBeats;
    vp.p.findTrack(dt)->audioClips.push_back(c);
    REQUIRE(vp.run("DoubleMagnet", {{"mainClipId", vp.clipId}, {"doubleClipId", c.id}, {"mode", "tight"}}));
    CHECK(vp.ctx->result["after_ms"].get<double>() < vp.ctx->result["before_ms"].get<double>() * 0.5);
    CHECK(vp.p.findAudioClip(c.id)->assetId != a.id);
    REQUIRE(vp.undo.undo());
    CHECK(vp.p.findAudioClip(c.id)->assetId == a.id);
}
