// GMB 14 - vocal note editor: manual note targets / keep-as-sung for Pitch Guardian, commands, undo, save/load.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "commands/Commands.h"
#include "project/ProjectIO.h"
#include "vocal/PitchDetector.h"
#include "vocal/PitchGuardian.h"

#include <algorithm>

using namespace roy;
using namespace roy::vocal;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;
double medianPitch(const PitchTrack& t, double t0, double t1) {
    std::vector<double> v;
    for (auto& f : t.frames)
        if (f.voiced && f.time >= t0 && f.time < t1) v.push_back(f.midi);
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
std::vector<float> twoNotes() {
    VoiceSpec v;
    v.seconds = 2.0;
    v.midi = [](double t) {
        if (t < 0.8) return 60.42; // C4 +42 c
        if (t < 1.0) return 0.0;
        return 64.38;              // E4 +38 c
    };
    return makeVoice(v);
}
PitchGuardianSettings lockC() {
    PitchGuardianSettings s;
    s.mode = GuardianMode::Lock;
    s.key = Key{0, ScaleType::Major};
    return s;
}
} // namespace

TEST_CASE("gmb14", "manual note target and keep-as-sung win over the automatic correction") {
    const auto x = twoNotes();
    auto s = lockC();
    auto autoRes = runPitchGuardian({x}, SR, s);
    auto a = detectPitch(autoRes.audio[0].data(), static_cast<int64_t>(autoRes.audio[0].size()), SR);
    CHECK_NEAR(medianPitch(a, 0.15, 0.7), 60.0, 0.06);
    CHECK_NEAR(medianPitch(a, 1.15, 1.9), 64.0, 0.06);
    // note 1 -> D4 (62) by hand, note 2 kept as sung
    s.edits.push_back({0.0, 0.8, 62.0, false});
    s.edits.push_back({1.0, 2.0, 0.0, true});
    auto ed = runPitchGuardian({x}, SR, s);
    auto e = detectPitch(ed.audio[0].data(), static_cast<int64_t>(ed.audio[0].size()), SR);
    const double n1 = medianPitch(e, 0.15, 0.7), n2 = medianPitch(e, 1.15, 1.9);
    std::printf("    edited: note 1 %.3f (target 62), note 2 %.3f (kept, sung 64.38)\n", n1, n2);
    CHECK_NEAR(n1, 62.0, 0.06);
    CHECK_NEAR(n2, 64.38, 0.03);
    int manual = 0;
    for (auto& n : ed.plan.notes) manual += n.reason.rfind("manual", 0) == 0;
    CHECK(manual == 2);
    // kept note is bit-identical to the original
    double maxDiff = 0;
    for (size_t i = static_cast<size_t>(1.2 * SR); i < static_cast<size_t>(1.9 * SR); ++i) maxDiff = std::max(maxDiff, std::fabs(double(ed.audio[0][i]) - x[i]));
    CHECK(maxDiff == 0.0);
}

TEST_CASE("gmb14", "SetNoteEdit / ClearNoteEdits: one undo step each, replace per note, saved with the project") {
    Project p = makeNewProject("edits", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, nullptr};
    Track& t = addTrack(p, TrackType::Audio, "Vocal");
    AudioClip c;
    c.id = "clip1";
    c.assetId = "a";
    t.audioClips.push_back(c);
    REQUIRE(reg.execute(ctx, "SetNoteEdit", {{"clipId", "clip1"}, {"start", 0.1}, {"end", 0.7}, {"target", 62.0}}));
    REQUIRE(reg.execute(ctx, "SetNoteEdit", {{"clipId", "clip1"}, {"start", 1.0}, {"end", 1.8}, {"keep", true}}));
    REQUIRE(reg.execute(ctx, "SetNoteEdit", {{"clipId", "clip1"}, {"start", 0.1}, {"end", 0.7}, {"target", 61.5}})); // same note: replaced
    CHECK(p.findAudioClip("clip1")->noteEdits.size() == 2);
    CHECK(!reg.execute(ctx, "SetNoteEdit", {{"clipId", "clip1"}, {"start", 0.5}, {"end", 0.4}, {"target", 60.0}})); // invalid range
    // save / load keeps the edits
    Project q;
    std::string err;
    REQUIRE(deserializeProject(serializeProject(p), q, &err));
    const auto& qe = q.findAudioClip("clip1")->noteEdits;
    REQUIRE(qe.size() == 2);
    bool found = false;
    for (auto& e : qe) found = found || (e.value("target", 0.0) == 61.5);
    CHECK(found);
    undo.undo();
    double tgt = 0;
    for (auto& e : p.findAudioClip("clip1")->noteEdits) tgt = std::max(tgt, e.value("target", 0.0));
    CHECK(tgt == 62.0);
    REQUIRE(reg.execute(ctx, "ClearNoteEdits", {{"clipId", "clip1"}}));
    CHECK(p.findAudioClip("clip1")->noteEdits.empty());
    undo.undo();
    CHECK(p.findAudioClip("clip1")->noteEdits.size() == 2);
    // a project without edits writes no "noteEdits" key (old files unchanged)
    Project fresh = makeNewProject("fresh", SR, 120.0);
    addTrack(fresh, TrackType::Audio, "V").audioClips.push_back(c);
    CHECK(serializeProject(fresh).find("noteEdits") == std::string::npos);
}
