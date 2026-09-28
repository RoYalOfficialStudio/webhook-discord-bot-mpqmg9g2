// VOCAL TUNING UX: pitch editor data (waveform, pitch curve, detected/target notes, cents,
// confidence, IN_SCALE / OFF_KEY / UNCERTAIN / CORRECTED), A/B original vs. corrected,
// re-tuning always from the original, original file never modified. MOCK synthetic voice.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VocalMaterial.h"

#include "commands/Commands.h"
#include "core/Files.h"
#include "io/AudioFile.h"
#include "project/ProjectIO.h"

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
struct VocalProject {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("vox", 48000.0, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    fs::path dir, file;
    std::string clipId;
    explicit VocalProject(const VocalSpec& v, const std::string& name) {
        engine.prepare(48000.0, 256);
        registerCoreCommands(reg);
        dir = tempDir(name);
        fs::create_directories(dir / "Audio");
        rt.setProjectDirectory(dir);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt, dir});
        p.key = Key{0, ScaleType::Major};
        const auto take = makeVocal(v);
        file = dir / "take.wav";
        REQUIRE(writeWavFile(file, {take.audio}, take.sr, SampleFormat::Float32));
        const std::string track = addTrack(p, TrackType::Audio, "Lead").id;
        REQUIRE(run("ImportAudio", {{"path", file.string()}, {"trackId", track}}));
        clipId = ctx->result["clipId"];
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s failed: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    AudioClip& clip() { return *p.findAudioClip(clipId); }
};

VocalSpec spec() {
    VocalSpec v;
    // C (in tune), F# (off key in C major, clearly sung), E 40 cents flat, G quiet + noisy (uncertain)
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.5, 60.02}, {SegKind::Rest, 0.08}, {SegKind::Note, 0.5, 66.0},
              {SegKind::Rest, 0.08}, {SegKind::Note, 0.5, 63.6}, {SegKind::Rest, 0.1}};
    v.noiseDb = -70;
    return v;
}
const char* statusOf(const json& notes, double t) {
    for (auto& n : notes)
        if (n["start"].get<double>() <= t && n["end"].get<double>() >= t) return n["status"].get_ref<const std::string&>().c_str();
    return "none";
}
} // namespace

TEST_CASE("vocal-editor", "pitch editor data: waveform, curve, targets, cents and statuses; changes nothing") {
    VocalProject v(spec(), "vox_editor");
    const size_t undoBefore = v.undo.undoCount();
    REQUIRE(v.run("PitchEditorData", {{"clipId", v.clipId}, {"mode", "assist"}, {"offKeyFilter", true}, {"columns", 400}}));
    const json r = v.ctx->result;
    CHECK(r["waveMin"].size() == 400);
    CHECK(r["curve"].size() > 50);
    const json notes = r["notes"];
    REQUIRE(notes.size() >= 3);
    CHECK(std::string(statusOf(notes, 0.35)) == "IN_SCALE");
    CHECK(std::string(statusOf(notes, 1.5)) == "CORRECTED"); // 40 cents flat E -> E
    // F# is off key; with the OFF-KEY filter ASSIST pulls it into the key (or leaves it OFF_KEY): never silently "in scale"
    const std::string fs6 = statusOf(notes, 0.93);
    CHECK_MSG(fs6 == "OFF_KEY" || fs6 == "CORRECTED", fs6);
    for (auto& n : notes) {
        CHECK(n.contains("cents") && n.contains("confidence") && n.contains("target") && n.contains("detected"));
        if (n["status"] == "CORRECTED") CHECK(std::fabs(n["target"].get<double>() - std::round(n["target"].get<double>())) < 0.05);
    }
    CHECK(v.undo.undoCount() == undoBefore); // preview only
    CHECK(v.clip().rawAssetId.empty());
    CHECK(!r["ab"]["available"].get<bool>());
}

TEST_CASE("vocal-editor", "A/B: original stays untouched, switching is undoable, re-tuning starts from the original") {
    VocalProject v(spec(), "vox_ab");
    const std::string originalHash = files::sha256File(v.file);
    const std::string importedAsset = v.clip().assetId;
    CHECK(!v.run("VocalAB", {{"clipId", v.clipId}, {"use", "original"}})); // nothing tuned yet
    REQUIRE(v.run("PitchGuardian", {{"clipId", v.clipId}, {"mode", "assist"}, {"strength", 1.0}, {"speedMs", 5.0}}));
    REQUIRE(v.ctx->result["corrected"].get<int>() >= 1);
    CHECK(v.clip().rawAssetId == importedAsset);
    CHECK(v.clip().tunedAssetId == v.clip().assetId);
    const std::string tuned1 = v.clip().tunedAssetId;
    const std::string tuned1Hash = files::sha256File(v.dir / v.p.findAsset(tuned1)->path);
    // A: original
    REQUIRE(v.run("VocalAB", {{"clipId", v.clipId}, {"use", "original"}}));
    CHECK(v.clip().assetId == importedAsset);
    CHECK(v.clip().listeningOriginal());
    REQUIRE(v.run("PitchEditorData", {{"clipId", v.clipId}}));
    CHECK(v.ctx->result["ab"]["listening"] == "original");
    // B: corrected; undo goes back to A
    REQUIRE(v.run("VocalAB", {{"clipId", v.clipId}, {"use", "corrected"}}));
    CHECK(v.clip().assetId == tuned1);
    REQUIRE(v.undo.undo());
    CHECK(v.clip().assetId == importedAsset);
    REQUIRE(v.undo.redo());
    // re-tuning with the same settings from the ORIGINAL gives the same result (no stacked correction)
    REQUIRE(v.run("PitchGuardian", {{"clipId", v.clipId}, {"mode", "assist"}, {"strength", 1.0}, {"speedMs", 5.0}}));
    const std::string tuned2 = v.clip().tunedAssetId;
    CHECK(tuned2 != tuned1);
    CHECK(v.clip().rawAssetId == importedAsset);
    CHECK(files::sha256File(v.dir / v.p.findAsset(tuned2)->path) == tuned1Hash);
    // the recorded / imported file itself was never modified
    CHECK(files::sha256File(v.file) == originalHash);
    CHECK(files::sha256File(v.dir / v.p.findAsset(importedAsset)->path) == originalHash);
    // lineage survives save / reload
    REQUIRE(saveProject(v.p, v.dir / "vox.roy").ok);
    Project q;
    REQUIRE(loadProject(v.dir / "vox.roy", q).ok);
    CHECK(q.findAudioClip(v.clipId)->rawAssetId == importedAsset);
    CHECK(q.findAudioClip(v.clipId)->tunedAssetId == tuned2);
    // other processing makes the A/B pair stale: it is cleared, not left pointing at old audio
    REQUIRE(v.run("RegionGain", {{"clipId", v.clipId}, {"start", 0.1}, {"end", 0.3}, {"gainDb", -6.0}}));
    CHECK(v.clip().assetId != tuned2);
    CHECK(v.clip().rawAssetId.empty());
}

TEST_CASE("vocal-editor", "low confidence notes are shown UNCERTAIN and not corrected aggressively") {
    VocalSpec v;
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.5, 60.4, 0, 5.5, 0, 0.3}, {SegKind::Rest, 0.1}};
    v.breathiness = 0.25; // breathy, noisy: reduced periodicity
    v.noiseDb = -40;
    VocalProject vp(v, "vox_uncertain");
    REQUIRE(vp.run("PitchEditorData", {{"clipId", vp.clipId}, {"mode", "lock"}, {"minConfidence", 0.99}}));
    int uncertain = 0, corrected = 0;
    for (auto& n : vp.ctx->result["notes"]) {
        uncertain += n["status"] == "UNCERTAIN";
        corrected += n["status"] == "CORRECTED";
    }
    CHECK(uncertain + corrected >= 1);
    CHECK(corrected == 0);
}
