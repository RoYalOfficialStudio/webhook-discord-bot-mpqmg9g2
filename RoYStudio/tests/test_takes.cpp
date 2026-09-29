// TAKE LANES + COMPING via commands (what the playlist take lanes use): swipe-comp a range,
// whole take, rename, delete, clear, flatten - each one undo step, audible in the render.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "arrange/WaveformCache.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "io/AudioFile.h"
#include "record/Takes.h"

#include <format>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0; // 120 BPM: 1 beat = 0.5 s = 24000 frames

struct Comp {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("takes", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string track;
    Comp() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        Track& t = addTrack(p, TrackType::Audio, "Vocal");
        track = t.id;
        // two 4-beat takes of the same 440 Hz tone: quiet (0.1) and loud (0.6)
        const std::string a = addMemoryAsset(p, rt, makeSine(SR, 440, 2.0, 0.1f), "quiet");
        const std::string b = addMemoryAsset(p, rt, makeSine(SR, 440, 2.0, 0.6f), "loud");
        t.takes = {{"A", a, "Take 1", 0, 0.0, 4.0, "", 0}, {"B", b, "Take 2", 1, 0.0, 4.0, "", 0}};
    }
    Track& t() { return *p.findTrack(track); }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    // peak of the left channel inside each beat 0..3 (a guard band avoids the comp crossfades)
    std::vector<float> beatPeaks() {
        REQUIRE(rt.rebuild(p));
        auto o = renderOffline(engine, 4 * 24000);
        std::vector<float> v;
        for (int b = 0; b < 4; ++b) v.push_back(peak(o[0], static_cast<size_t>(b * 24000 + 3000), static_cast<size_t>(b * 24000 + 21000)));
        return v;
    }
    std::vector<std::vector<float>> renderOffline(AudioEngine& e, int64_t frames) {
        e.transport().seek(0);
        e.transport().play();
        return roytest::render(e, frames, 256);
    }
};
} // namespace

TEST_CASE("takes", "swipe comp: a range plays from the chosen take, undo restores the previous comp") {
    Comp c;
    REQUIRE(c.run("CompWholeTake", {{"trackId", c.track}, {"takeId", "A"}}));
    auto v = c.beatPeaks();
    for (float x : v) CHECK_NEAR(x, 0.1f, 0.01f);
    REQUIRE(c.run("CompSelect", {{"trackId", c.track}, {"takeId", "B"}, {"startBeat", 1.0}, {"endBeat", 3.0}}));
    REQUIRE(c.t().comp.size() == 3);
    v = c.beatPeaks();
    CHECK_NEAR(v[0], 0.1f, 0.01f);
    CHECK_NEAR(v[1], 0.6f, 0.02f);
    CHECK_NEAR(v[2], 0.6f, 0.02f);
    CHECK_NEAR(v[3], 0.1f, 0.01f);
    c.undo.undo();
    CHECK(c.t().comp.size() == 1);
    CHECK(c.t().comp[0].takeId == "A");
    c.undo.redo();
    CHECK(c.t().comp.size() == 3);
    // invalid input is rejected without an undo step
    const size_t steps = c.undo.undoCount();
    CHECK(!c.run("CompSelect", {{"trackId", c.track}, {"takeId", "B"}, {"startBeat", 6.0}, {"endBeat", 8.0}})); // outside the take
    CHECK(!c.run("CompSelect", {{"trackId", c.track}, {"takeId", "B"}, {"startBeat", 2.0}, {"endBeat", 2.0}}));
    CHECK(!c.run("CompSelect", {{"trackId", c.track}, {"takeId", "nope"}, {"startBeat", 0.0}, {"endBeat", 1.0}}));
    CHECK(c.undo.undoCount() == steps);
}

TEST_CASE("takes", "delete / rename / clear: model and audio follow, undo brings everything back") {
    Comp c;
    REQUIRE(c.run("CompWholeTake", {{"trackId", c.track}, {"takeId", "A"}}));
    REQUIRE(c.run("CompSelect", {{"trackId", c.track}, {"takeId", "B"}, {"startBeat", 2.0}, {"endBeat", 4.0}}));
    REQUIRE(c.run("RenameTake", {{"trackId", c.track}, {"takeId", "B"}, {"name", "Best Hook"}}));
    CHECK(c.t().takes[1].name == "Best Hook");
    CHECK(!c.run("RenameTake", {{"trackId", c.track}, {"takeId", "B"}, {"name", ""}}));
    REQUIRE(c.run("DeleteTake", {{"trackId", c.track}, {"takeId", "B"}}));
    CHECK(c.t().takes.size() == 1);
    CHECK(c.t().comp.size() == 1); // B's segment is gone, A keeps beats 0-2
    auto v = c.beatPeaks();
    CHECK_NEAR(v[0], 0.1f, 0.01f);
    CHECK(v[3] < 0.001f); // nothing plays where only the deleted take was comped
    c.undo.undo();
    CHECK(c.t().takes.size() == 2);
    CHECK(c.t().takes[1].name == "Best Hook");
    CHECK_NEAR(c.beatPeaks()[3], 0.6f, 0.02f);
    REQUIRE(c.run("ClearComp", {{"trackId", c.track}}));
    CHECK(c.t().comp.empty());
    for (float x : c.beatPeaks()) CHECK(x < 0.001f);
    c.undo.undo();
    CHECK(c.t().comp.size() == 2);
    // a locked track refuses comp edits
    c.t().locked = true;
    CHECK(!c.run("CompWholeTake", {{"trackId", c.track}, {"takeId", "B"}}));
    CHECK(!c.run("DeleteTake", {{"trackId", c.track}, {"takeId", "A"}}));
}

TEST_CASE("takes", "flatten comp: clips replace the comp with identical audio, one undo step") {
    Comp c;
    REQUIRE(c.run("CompWholeTake", {{"trackId", c.track}, {"takeId", "A"}}));
    REQUIRE(c.run("CompSelect", {{"trackId", c.track}, {"takeId", "B"}, {"startBeat", 1.5}, {"endBeat", 2.5}}));
    REQUIRE(c.rt.rebuild(c.p));
    const auto before = c.renderOffline(c.engine, 4 * 24000);
    REQUIRE(c.run("FlattenComp", {{"trackId", c.track}}));
    CHECK(c.ctx->result["clipIds"].size() == 3);
    CHECK(c.t().comp.empty());
    CHECK(c.t().audioClips.size() == 3);
    CHECK(c.t().takes.size() == 2); // takes stay for later re-comping
    REQUIRE(c.rt.rebuild(c.p));
    const auto after = c.renderOffline(c.engine, 4 * 24000);
    double m = 0;
    for (size_t i = 0; i < before[0].size(); ++i) m = std::max(m, std::fabs(double(before[0][i]) - after[0][i]));
    CHECK(m < 1e-6);
    CHECK(!c.run("FlattenComp", {{"trackId", c.track}})); // nothing left to flatten
    c.undo.undo();
    CHECK(c.t().comp.size() == 3);
    CHECK(c.t().audioClips.empty());
}

TEST_CASE("takes", "runtime asset cache: deleted takes and old stretch variants are released, pinned/used data stays") {
    const fs::path dir = roytest::tempDir("asset_prune");
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    rt.setProjectDirectory(dir);
    Project p = makeNewProject("prune", SR, 120.0);
    registerBuiltinProcessors();
    Track& t = addTrack(p, TrackType::Audio, "Vocal");
    // two takes with real files (reloadable) + one pinned in-memory asset on a clip
    std::vector<std::string> ids;
    for (int i = 0; i < 2; ++i) {
        const auto d = makeSine(SR, 330 + 110 * i, 2.0, 0.3f);
        const fs::path f = dir / std::format("take{}.wav", i);
        REQUIRE(writeWavFile(f, d->channels, SR, SampleFormat::Float32, false, nullptr));
        AudioAsset a;
        a.id = files::newId();
        a.path = f.string();
        a.sampleRate = SR;
        a.channels = 2;
        a.frames = d->numFrames;
        p.assets.push_back(a);
        ids.push_back(a.id);
        t.takes.push_back({std::format("T{}", i), a.id, std::format("Take {}", i + 1), i, 0.0, 4.0, "", 0});
    }
    const std::string mem = addMemoryAsset(p, rt, makeSine(SR, 220, 2.0, 0.2f), "pinned");
    addClip(t, mem, 8.0, 4.0).stretch = 1.25; // stretched: a derived variant is cached
    REQUIRE(takes::compWholeTake(t, "T0"));
    REQUIRE(takes::compSelect(t, "T1", 2.0, 4.0));
    REQUIRE(rt.rebuild(p));
    const size_t full = rt.cachedAssetCount();
    CHECK(full >= 4); // 2 takes + pinned + derived
    // delete take 2 -> its audio leaves the cache (the graph is rebuilt twice: old graph released)
    REQUIRE(takes::removeTake(t, "T1"));
    REQUIRE(rt.rebuild(p));
    engine.collectGarbage();
    REQUIRE(rt.rebuild(p));
    CHECK(rt.cachedAssetCount() == full - 1);
    // change the stretch: the old variant goes, the new one is cached
    t.audioClips[0].stretch = 1.5;
    REQUIRE(rt.rebuild(p));
    engine.collectGarbage();
    REQUIRE(rt.rebuild(p));
    CHECK(rt.cachedAssetCount() == full - 1);
    // a muted stretched clip keeps its variant (unmute must not re-stretch)
    t.audioClips[0].muted = true;
    REQUIRE(rt.rebuild(p));
    engine.collectGarbage();
    REQUIRE(rt.rebuild(p));
    CHECK(rt.cachedAssetCount() == full - 1);
    // everything removed from the model: only the pinned in-memory asset stays
    t.audioClips.clear();
    t.takes.clear();
    t.comp.clear();
    REQUIRE(rt.rebuild(p));
    engine.collectGarbage();
    REQUIRE(rt.rebuild(p));
    CHECK(rt.cachedAssetCount() == 1);
    // undo-style restore: the file-backed take reloads transparently
    t.takes.push_back({"T0", ids[0], "Take 1", 0, 0.0, 4.0, "", 0});
    REQUIRE(takes::compWholeTake(t, "T0"));
    REQUIRE(rt.rebuild(p));
    CHECK(rt.lastWarnings().empty());
    engine.transport().seek(0);
    engine.transport().play();
    const auto o = roytest::render(engine, 24000, 256);
    CHECK(peak(o[0], 2000, 22000) > 0.25f);
}

TEST_CASE("takes", "waveform store keeps only peaks of assets the project still references") {
    Comp c;
    WaveformStore store; // no cache directory: memory only
    for (auto& k : c.t().takes) REQUIRE(store.get(k.assetId, *c.rt.asset(c.p, k.assetId)));
    CHECK(store.size() == 2);
    CHECK(referencedAssetIds(c.p).size() == 2);
    REQUIRE(c.run("DeleteTake", {{"trackId", c.track}, {"takeId", "B"}}));
    CHECK(store.retainOnly(referencedAssetIds(c.p)) == 1);
    CHECK(store.size() == 1);
    c.undo.undo(); // take is back -> its peaks are simply rebuilt on the next draw
    CHECK(referencedAssetIds(c.p).size() == 2);
    CHECK(store.retainOnly(referencedAssetIds(c.p)) == 0);
}
