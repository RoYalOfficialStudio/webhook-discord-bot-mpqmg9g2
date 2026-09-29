// BEAT LAB: groove templates (non-destructive), velocity curves, note repeat / ratchets /
// triplet rolls, flams, probability, generator styles, variations, pattern chaining, and
// the undo contract: one user action = one undo step (100 tracks, generation, multi-clip move).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "beat/StepSequencer.h"
#include "commands/Commands.h"
#include "project/ProjectIO.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

struct Beat {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("beat", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string patId, trackId;
    Beat() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        REQUIRE(run("AddPattern", {{"steps", 16}}));
        patId = ctx->result["id"];
        REQUIRE(run("AddTrack", {{"type", "beat"}, {"name", "Drums"}}));
        trackId = ctx->result["id"];
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s failed: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    Pattern& pat() { return *p.findPattern(patId); }
    PatternRow& row(const std::string& voice) {
        for (auto& r : pat().rows)
            if (r.voice == voice) return r;
        return pat().rows.front();
    }
    std::vector<PatternNote> notes(int noteNumber = -1) {
        PatternClip c;
        c.patternId = patId;
        c.lengthBeats = pat().lengthBeats();
        auto all = expandPatternClip(pat(), c, 42);
        if (noteNumber >= 0) std::erase_if(all, [&](const PatternNote& n) { return n.note != noteNumber; });
        return all;
    }
};
} // namespace

TEST_CASE("beatlab", "groove templates shift timing and accents on playback without touching the steps") {
    Beat b;
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"text", "xxxxxxxxxxxxxxxx"}}));
    const int hat = b.row("closed_hat").note;
    const auto straight = b.notes(hat);
    REQUIRE(straight.size() == 16);
    const auto stepsBefore = b.row("closed_hat").steps;
    CHECK(!b.run("SetPatternGroove", {{"patternId", b.patId}, {"groove", "No Such Groove"}}));
    REQUIRE(b.run("SetPatternGroove", {{"patternId", b.patId}, {"groove", "MPC 62%"}, {"amount", 1.0}}));
    auto g = b.notes(hat);
    REQUIRE(g.size() == 16);
    CHECK_NEAR(g[0].beat, 0.0, 1e-9);
    CHECK_NEAR(g[1].beat - straight[1].beat, 0.24 * 0.25, 1e-6); // off-beat 16th at 62% of the 8th
    CHECK(g[1].velocity < g[0].velocity);
    REQUIRE(b.run("SetPatternGroove", {{"patternId", b.patId}, {"amount", 0.5}}));
    g = b.notes(hat);
    CHECK_NEAR(g[1].beat - straight[1].beat, 0.12 * 0.25, 1e-6);
    // non-destructive: steps unchanged; saved and restored with the project
    const auto& after = b.row("closed_hat").steps;
    bool same = after.size() == stepsBefore.size();
    for (size_t i = 0; same && i < after.size(); ++i) same = after[i].on == stepsBefore[i].on && after[i].velocity == stepsBefore[i].velocity;
    CHECK(same);
    Project q;
    std::string err;
    REQUIRE_MSG_OK(projectFromJson(projectToJson(b.p), q, &err), err);
    CHECK(q.findPattern(b.patId)->groove == "MPC 62%");
    CHECK_NEAR(q.findPattern(b.patId)->grooveAmount, 0.5, 1e-6);
    // every template is well formed
    for (auto& t : beat::grooveTemplates()) {
        bool ok = !t.name.empty() && !t.description.empty();
        for (int i = 0; i < 16; ++i) ok &= std::fabs(t.timing[i]) < 0.5f && t.velocity[i] > 0.3f && t.velocity[i] <= 1.0f;
        CHECK_MSG(ok, t.name);
    }
}

TEST_CASE("beatlab", "velocity curves shape only active steps, one undo step") {
    Beat b;
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"text", "x.xxxxxxxxxxxxxx"}}));
    const size_t undoBefore = b.undo.undoCount();
    REQUIRE(b.run("SetVelocityCurve", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"curve", "Ramp Up"}, {"lo", 0.2}, {"hi", 1.0}}));
    CHECK(b.undo.undoCount() == undoBefore + 1);
    const auto& st = b.row("closed_hat").steps;
    CHECK_NEAR(st[0].velocity, 0.2, 1e-6);
    CHECK_NEAR(st[15].velocity, 1.0, 1e-6);
    CHECK(!st[1].on);
    bool rising = true;
    for (int i = 3; i < 16; ++i) rising &= st[static_cast<size_t>(i)].velocity >= st[static_cast<size_t>(i - 1)].velocity;
    CHECK(rising);
    REQUIRE(b.run("SetVelocityCurve", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"curve", "Accent Downbeats"}, {"lo", 0.5}, {"hi", 0.9}}));
    CHECK_NEAR(b.row("closed_hat").steps[4].velocity, 0.9, 1e-6);
    CHECK_NEAR(b.row("closed_hat").steps[5].velocity, 0.5, 1e-6);
    REQUIRE(b.run("SetVelocityCurve", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"curve", "Humanize"}, {"lo", 0.4}, {"hi", 0.95}, {"seed", 3}}));
    for (auto& s : b.row("closed_hat").steps)
        if (s.on) CHECK(s.velocity >= 0.4f && s.velocity <= 0.95f);
    CHECK(!b.run("SetVelocityCurve", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"curve", "Wobble"}}));
    REQUIRE(b.undo.undo());
    CHECK_NEAR(b.row("closed_hat").steps[4].velocity, 0.9, 1e-6);
}

TEST_CASE("beatlab", "note repeat: straight rates, ratchets and triplet rolls land on the right grid") {
    Beat b;
    const int hat = b.row("closed_hat").note;
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/8"}, {"velocity", 0.7}}));
    auto n = b.notes(hat);
    REQUIRE(n.size() == 8);
    for (size_t i = 0; i < n.size(); ++i) CHECK_NEAR(n[i].beat, 0.5 * i, 1e-9);
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/32"}}));
    n = b.notes(hat);
    REQUIRE(n.size() == 32);
    for (size_t i = 0; i < n.size(); ++i) CHECK_NEAR(n[i].beat, 0.125 * i, 1e-9);
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/16T"}}));
    n = b.notes(hat);
    REQUIRE(n.size() == 24); // 3 hits per 2 steps
    for (size_t i = 0; i < n.size(); ++i) CHECK_NEAR(n[i].beat, i / 6.0, 1e-9);
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/8T"}}));
    n = b.notes(hat);
    REQUIRE(n.size() == 12);
    for (size_t i = 0; i < n.size(); ++i) CHECK_NEAR(n[i].beat, i / 3.0, 1e-9);
    // partial range: steps 8..11 only, the rest keeps its content
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"text", "x.x.x.x.x.x.x.x."}}));
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/64"}, {"from", 8}, {"to", 11}}));
    n = b.notes(hat);
    CHECK(n.size() == 4 + 16 + 2);
    CHECK(!b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/5"}}));
    // ratchet + triplet roll survive save/reload
    Project q;
    std::string err;
    REQUIRE(b.run("NoteRepeat", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"rate", "1/16T"}}));
    REQUIRE_MSG_OK(projectFromJson(projectToJson(b.p), q, &err), err);
    for (auto& r : q.findPattern(b.patId)->rows)
        if (r.voice == "closed_hat") {
            CHECK(r.steps[0].roll == 3);
            CHECK(r.steps[0].rollLength == 2);
        }
}

TEST_CASE("beatlab", "flam and probability: grace note before the hit, probability deterministic per seed") {
    Beat b;
    const int snare = b.row("snare").note;
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "snare"}, {"text", "....x.......x..."}}));
    REQUIRE(b.run("SetStep", {{"patternId", b.patId}, {"voice", "snare"}, {"step", 4}, {"on", true}, {"flam", true}}));
    auto n = b.notes(snare);
    REQUIRE(n.size() == 3);
    CHECK(n[0].beat < 1.0 && n[0].beat > 0.9);
    CHECK(n[0].velocity < n[1].velocity);
    // probability 0.5 on 16 hats over 64 bars
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"text", "xxxxxxxxxxxxxxxx"}}));
    for (int s = 0; s < 16; ++s) REQUIRE(b.run("SetStep", {{"patternId", b.patId}, {"voice", "closed_hat"}, {"step", s}, {"on", true}, {"probability", 0.5}}));
    PatternClip c;
    c.patternId = b.patId;
    c.lengthBeats = 4.0 * 64;
    const int hat = b.row("closed_hat").note;
    auto count = [&](uint64_t seed) {
        auto all = expandPatternClip(b.pat(), c, seed);
        return std::count_if(all.begin(), all.end(), [&](auto& x) { return x.note == hat; });
    };
    const auto c1 = count(7);
    CHECK(c1 == count(7));      // same seed -> same result (renders are reproducible)
    CHECK(c1 > 1024 * 0.4 && c1 < 1024 * 0.6);
    CHECK(count(8) != c1);      // another seed -> another take
}

TEST_CASE("beatlab", "generator styles, variations and pattern chains - each is one undo step") {
    Beat b;
    for (auto& style : beat::generatorStyles()) {
        const size_t before = b.p.patterns.size(), undoBefore = b.undo.undoCount();
        REQUIRE(b.run("GeneratePattern", {{"style", style}, {"seed", 11}}));
        CHECK(b.p.patterns.size() == before + 1);
        CHECK(b.undo.undoCount() == undoBefore + 1);
        const Pattern& g = b.p.patterns.back();
        CHECK(beat::findGroove(g.groove) != nullptr);
        int kicks = 0, backbeat = 0;
        for (auto& r : g.rows)
            for (size_t i = 0; i < r.steps.size(); ++i) {
                if (!r.steps[i].on) continue;
                if (r.voice == "kick") ++kicks;
                if (r.voice == "snare" || r.voice == "clap" || r.voice == "rim") ++backbeat;
            }
        CHECK_MSG(kicks >= 2 && backbeat >= 1, style);
    }
    // deterministic per seed
    REQUIRE(b.run("GeneratePattern", {{"style", "Trap"}, {"seed", 5}}));
    const json a = projectToJson(b.p)["patterns"].back()["rows"];
    REQUIRE(b.run("GeneratePattern", {{"style", "Trap"}, {"seed", 5}}));
    json bb = projectToJson(b.p)["patterns"].back()["rows"];
    for (auto& r : bb) r.erase("id");
    json aa = a;
    for (auto& r : aa) r.erase("id");
    CHECK(aa == bb);
    CHECK(!b.run("GeneratePattern", {{"style", "Polka"}}));
    REQUIRE(b.undo.undo());
    CHECK(b.p.patterns.size() == 2 + beat::generatorStyles().size());

    // variation
    REQUIRE(b.run("MakeVariation", {{"patternId", b.patId}, {"seed", 3}, {"name", "Verse B"}}));
    CHECK(b.p.patterns.back().name == "Verse B");
    const std::string var = b.ctx->result["id"];

    // chain A B A, twice, back to back - one undo step
    const size_t undoBefore = b.undo.undoCount();
    REQUIRE(b.run("PlacePatternChain", {{"trackId", b.trackId}, {"patternIds", {b.patId, var, b.patId}}, {"startBeat", 8.0}, {"repeats", 2}}));
    CHECK(b.undo.undoCount() == undoBefore + 1);
    const auto& clips = b.p.findTrack(b.trackId)->patternClips;
    REQUIRE(clips.size() == 6);
    for (size_t i = 0; i < clips.size(); ++i) CHECK_NEAR(clips[i].startBeat, 8.0 + 4.0 * i, 1e-9);
    CHECK(clips[1].patternId == var);
    CHECK_NEAR(b.ctx->result["endBeat"].get<double>(), 32.0, 1e-9);
    CHECK(!b.run("PlacePatternChain", {{"trackId", b.trackId}, {"patternIds", {b.patId, "missing"}}}));
    CHECK(b.p.findTrack(b.trackId)->patternClips.size() == 6); // a failed chain places nothing
    REQUIRE(b.undo.undo());
    CHECK(b.p.findTrack(b.trackId)->patternClips.empty());
}

TEST_CASE("beatlab", "undo macros: 100 tracks, moving several clips - one undo step each, all or nothing") {
    Beat b;
    std::vector<std::pair<std::string, json>> steps;
    for (int i = 0; i < 100; ++i) steps.push_back({"AddTrack", {{"type", i % 2 ? "midi" : "audio"}, {"name", std::format("T{}", i)}}});
    const size_t tracksBefore = b.p.tracks.size(), undoBefore = b.undo.undoCount();
    REQUIRE(b.reg.executeMacro(*b.ctx, "Add 100 tracks", steps));
    CHECK(b.p.tracks.size() == tracksBefore + 100);
    CHECK(b.undo.undoCount() == undoBefore + 1);
    REQUIRE(b.undo.undo());
    CHECK(b.p.tracks.size() == tracksBefore);
    REQUIRE(b.undo.redo());
    CHECK(b.p.tracks.size() == tracksBefore + 100);

    // several clips moved together
    REQUIRE(b.run("PlacePatternChain", {{"trackId", b.trackId}, {"patternIds", {b.patId, b.patId, b.patId}}}));
    std::vector<std::string> ids;
    for (auto& c : b.p.findTrack(b.trackId)->patternClips) ids.push_back(c.id);
    const size_t u = b.undo.undoCount();
    REQUIRE(b.run("MoveClips", {{"clipIds", ids}, {"deltaBeats", 16.0}}));
    CHECK(b.undo.undoCount() == u + 1);
    const auto& clips = b.p.findTrack(b.trackId)->patternClips;
    for (size_t i = 0; i < clips.size(); ++i) CHECK_NEAR(clips[i].startBeat, 16.0 + 4.0 * i, 1e-9);
    // invalid request (one clip missing / before song start) changes nothing
    CHECK(!b.run("MoveClips", {{"clipIds", {ids[0], "missing"}}, {"deltaBeats", 4.0}}));
    CHECK(!b.run("MoveClips", {{"clipIds", ids}, {"deltaBeats", -100.0}}));
    CHECK_NEAR(b.p.findTrack(b.trackId)->patternClips[0].startBeat, 16.0, 1e-9);
    CHECK(b.undo.undoCount() == u + 1);
    REQUIRE(b.undo.undo());
    CHECK_NEAR(b.p.findTrack(b.trackId)->patternClips[0].startBeat, 0.0, 1e-9);
    // a macro whose last step fails leaves no trace
    const size_t n = b.p.tracks.size();
    CHECK(!b.reg.executeMacro(*b.ctx, "Broken", {{"AddTrack", {{"type", "audio"}}}, {"DeleteTrack", {{"trackId", "nope"}}}}));
    CHECK(b.p.tracks.size() == n);
}

TEST_CASE("beatlab", "delete pattern removes its playlist clips, keeps others, one undo brings both back") {
    Beat b;
    REQUIRE(b.run("SetRowPattern", {{"patternId", b.patId}, {"voice", "kick"}, {"text", "x...x...x...x..."}}));
    REQUIRE(b.run("AddPattern", {{"steps", 32}, {"name", "Keep"}}));
    const std::string keep = b.ctx->result["id"];
    REQUIRE(b.run("PlacePatternChain", {{"trackId", b.trackId}, {"patternIds", {b.patId, keep, b.patId}}}));
    REQUIRE(b.p.findTrack(b.trackId)->patternClips.size() == 3);
    const size_t u = b.undo.undoCount();
    REQUIRE(b.run("DeletePattern", {{"patternId", b.patId}}));
    CHECK(b.ctx->result.value("removedClips", 0) == 2);
    CHECK(b.undo.undoCount() == u + 1);
    CHECK(b.p.findPattern(b.patId) == nullptr);
    REQUIRE(b.p.patterns.size() == 1);
    REQUIRE(b.p.findTrack(b.trackId)->patternClips.size() == 1);
    CHECK(b.p.findTrack(b.trackId)->patternClips[0].patternId == keep);
    CHECK(!b.run("DeletePattern", {{"patternId", b.patId}})); // already gone: error, no undo step
    CHECK(b.undo.undoCount() == u + 1);
    REQUIRE(b.undo.undo());
    REQUIRE(b.p.findPattern(b.patId) != nullptr);
    CHECK(b.p.findTrack(b.trackId)->patternClips.size() == 3);
    CHECK(b.row("kick").steps[4].on); // the steps come back too
    // locked track protects its clips
    REQUIRE(b.run("LockTrack", {{"trackId", b.trackId}, {"locked", true}}));
    CHECK(!b.run("DeletePattern", {{"patternId", b.patId}}));
    CHECK(b.p.findPattern(b.patId) != nullptr);
    // rename
    REQUIRE(b.run("RenamePattern", {{"patternId", keep}, {"name", "Hook"}}));
    CHECK(b.p.findPattern(keep)->name == "Hook");
    CHECK(!b.run("RenamePattern", {{"patternId", keep}, {"name", ""}}));
}
