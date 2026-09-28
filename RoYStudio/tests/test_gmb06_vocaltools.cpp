// GMB 06 - Vocal Doctor, Microscope, Double Magnet, Ghost Take, Flow Analyzer.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "dsp/Analysis.h"
#include "dsp/Filters.h"
#include "vocal/FlowAnalyzer.h"
#include "vocal/PitchDetector.h"
#include "vocal/VocalDoctor.h"
#include "vocal/VocalTools.h"

using namespace roy;
using namespace roy::vocal;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

bool hasIssue(const DoctorReport& r, const std::string& id) {
    for (auto& i : r.issues)
        if (i.id == id) return true;
    return false;
}

// "Syllables": short voiced bursts at the given times (seconds), 140 ms each.
std::vector<float> syllables(const std::vector<double>& times, double total, double midi = 57.0, double centsOffset = 0.0) {
    VoiceSpec v;
    v.seconds = total;
    v.midi = [times, midi, centsOffset](double t) {
        for (double s : times)
            if (t >= s && t < s + 0.14) return midi + centsOffset / 100.0 + (t - s) * 1.5; // slight rise
        return 0.0;
    };
    auto x = makeVoice(v);
    // short attack/decay envelopes so onsets are clear
    for (double s : times) {
        const size_t a = static_cast<size_t>(s * SR);
        for (size_t i = 0; i < static_cast<size_t>(0.14 * SR) && a + i < x.size(); ++i) {
            const double t = static_cast<double>(i) / SR;
            const double env = std::min(1.0, t / 0.004) * std::min(1.0, (0.14 - t) / 0.02);
            x[a + i] *= static_cast<float>(env);
        }
    }
    return x;
}
} // namespace

TEST_CASE("gmb06", "vocal doctor finds technical problems") {
    VoiceSpec v;
    v.seconds = 3.0;
    v.midi = [](double t) { return std::fmod(t, 1.0) < 0.7 ? 57.0 + (t > 1.5 ? 2 : 0) : 0.0; };
    auto clean = makeVoice(v);
    auto rc = examineVocal({clean}, SR);
    CHECK(!hasIssue(rc, "clipping"));
    CHECK(!hasIssue(rc, "dc_offset"));
    CHECK(!hasIssue(rc, "rumble"));
    CHECK(rc.m.pitchMedianMidi > 56.5 && rc.m.pitchMedianMidi < 59.5);

    auto dirty = clean;
    Rng rng(9);
    dsp::Biquad hp;
    hp.set(dsp::Biquad::Type::HighPass, SR, 6000, 0.7);
    for (size_t i = 0; i < dirty.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        dirty[i] += 0.012f;                                                           // DC offset
        dirty[i] += 0.08f * static_cast<float>(std::sin(kTwoPi * 45.0 * t));          // rumble
        dirty[i] += static_cast<float>(rng.uniform(-0.004, 0.004));                   // noise floor ~ -52 dBFS
        if (t > 0.3 && t < 0.4) dirty[i] += 0.6f * hp.process(static_cast<float>(rng.uniform(-1, 1))); // "sss"
        if (t > 2.05 && t < 2.08) dirty[i] = std::clamp(dirty[i] * 8.0f, -1.0f, 1.0f); // clipping
    }
    auto r = examineVocal({dirty}, SR);
    CHECK_NEAR(r.m.dcOffset, 0.012, 0.003);
    CHECK(hasIssue(r, "dc_offset"));
    CHECK(hasIssue(r, "rumble"));
    CHECK(hasIssue(r, "clipping"));
    CHECK(r.m.clipEvents > 0);
    CHECK(hasIssue(r, "sibilance"));
    CHECK(hasIssue(r, "noise"));
    // every fix is expressed as a command (undoable through the command system)
    for (auto& i : r.issues)
        for (auto& f : i.fixes) CHECK(!f.command.empty());
    auto j = r.toJson();
    CHECK(j["issues"].size() == r.issues.size());
}

TEST_CASE("gmb06", "vocal doctor detects breaths and plosives") {
    VoiceSpec v;
    v.seconds = 2.0;
    v.midi = [](double t) { return (t < 0.6 || t > 1.2) ? 60.0 : 0.0; };
    auto x = makeVoice(v);
    Rng rng(4);
    dsp::Biquad bp;
    bp.set(dsp::Biquad::Type::BandPass, SR, 1500, 0.5);
    for (size_t i = static_cast<size_t>(0.75 * SR); i < static_cast<size_t>(1.05 * SR); ++i)
        x[i] += 0.05f * bp.process(static_cast<float>(rng.uniform(-1, 1))); // breath: noisy, quiet, unvoiced
    for (size_t i = static_cast<size_t>(1.6 * SR); i < static_cast<size_t>(1.64 * SR); ++i)
        x[i] += 0.7f * static_cast<float>(std::sin(kTwoPi * 60.0 * (static_cast<double>(i) / SR - 1.6))); // "p" pop
    auto r = examineVocal({x}, SR);
    REQUIRE(!r.m.breaths.empty());
    CHECK(r.m.breaths[0].first > 0.6 && r.m.breaths[0].second < 1.2);
    CHECK(hasIssue(r, "breaths"));
    CHECK(r.m.plosiveEvents >= 1);
}

TEST_CASE("gmb06", "vocal microscope inspects a region") {
    VoiceSpec v;
    v.seconds = 1.5;
    v.midi = [](double t) { return t < 1.0 ? 55.0 : 0.0; };
    auto x = makeVoice(v);
    Rng rng(5);
    for (size_t i = static_cast<size_t>(1.1 * SR); i < static_cast<size_t>(1.4 * SR); ++i) x[i] += static_cast<float>(rng.uniform(-0.02, 0.02));
    auto r = inspectRegion({x}, SR, 0.2, 0.8);
    CHECK_NEAR(r.pitchMedianMidi, 55.0, 0.1);
    CHECK(r.pitchStabilityCents < 10.0);
    CHECK(r.voicedRatio > 0.9);
    REQUIRE(r.formantsHz.size() >= 2);
    CHECK_NEAR(r.formantsHz[0], 700.0, 200.0);
    CHECK(!r.looksLikeBreath);
    auto b = inspectRegion({x}, SR, 1.1, 1.4);
    CHECK(b.looksLikeBreath);
    CHECK(b.voicedRatio < 0.1);
    // region-only edits
    auto g = applyRegionGain({x}, SR, 0.4, 0.6, -12.0);
    CHECK(g[0][static_cast<size_t>(0.2 * SR)] == x[static_cast<size_t>(0.2 * SR)]);
    CHECK_NEAR(g[0][static_cast<size_t>(0.5 * SR)], x[static_cast<size_t>(0.5 * SR)] * dbToGain(-12.0), 1e-6);
    auto p = applyRegionPitchShift({x}, SR, 0.4, 0.7, 2.0);
    auto tr = detectPitch(p[0].data(), static_cast<int64_t>(p[0].size()), SR);
    CHECK_NEAR(tr.midiAt(0.55), 57.0, 0.1);
    CHECK_NEAR(tr.midiAt(0.2), 55.0, 0.05);
}

TEST_CASE("gmb06", "double magnet tightens a sloppy double") {
    std::vector<double> t = {0.2, 0.55, 0.9, 1.25, 1.6, 1.95, 2.3, 2.65};
    std::vector<double> sloppy;
    Rng rng(11);
    for (double s : t) sloppy.push_back(s + rng.uniform(-0.06, 0.06));
    auto mainV = syllables(t, 3.0, 57.0);
    auto dbl = syllables(sloppy, 3.0, 57.0, 25.0);
    for (auto mode : {MagnetMode::Loose, MagnetMode::Tight, MagnetMode::UltraTight}) {
        MagnetSettings s;
        s.mode = mode;
        s.alignPitch = mode == MagnetMode::UltraTight;
        auto res = alignDouble({mainV}, {dbl}, SR, s);
        REQUIRE(res.audio.size() == 1);
        CHECK(res.audio[0].size() == dbl.size());
        CHECK(allFinite(res.audio[0]));
        CHECK_MSG(res.report.meanAbsOffsetBeforeMs > 15.0, std::format("before {}", res.report.meanAbsOffsetBeforeMs));
        if (mode == MagnetMode::Loose) {
            CHECK(res.report.meanAbsOffsetAfterMs < res.report.meanAbsOffsetBeforeMs);
        } else {
            CHECK_MSG(res.report.meanAbsOffsetAfterMs < 12.0, std::format("{} after {}", magnetModeId(mode), res.report.meanAbsOffsetAfterMs));
            CHECK(res.report.within20msAfter > res.report.within20msBefore);
        }
        if (s.alignPitch) CHECK(res.report.meanPitchDiffAfterCents < res.report.meanPitchDiffBeforeCents);
    }
    // originals untouched
    CHECK(syllables(sloppy, 3.0, 57.0, 25.0) == dbl);
}

TEST_CASE("gmb06", "ghost take compares timing and pitch") {
    std::vector<double> t = {0.2, 0.6, 1.0, 1.4, 1.8};
    std::vector<double> late;
    for (double s : t) late.push_back(s + 0.04);
    auto prev = syllables(t, 2.3, 60.0);
    auto cur = syllables(late, 2.3, 60.0, 30.0);
    auto g = compareTakes({prev}, {cur}, SR);
    CHECK_NEAR(g.meanAbsTimingMs, 40.0, 12.0);
    CHECK_NEAR(g.meanAbsPitchCents, 30.0, 10.0);
    CHECK(g.previousPeaks.size() == g.currentPeaks.size());
    CHECK(!g.previousPeaks.empty());
}

TEST_CASE("gmb06", "flow analyzer classifies early, on beat, late") {
    // 120 BPM: 1/16 = 0.125 s. Vocal starts at timeline 2.0 s (beat 4).
    TempoMap tempo(120.0);
    const double start = 2.0;
    // local times: on, on, late 40 ms, early 40 ms, on | pause | on, on
    std::vector<double> local = {0.0, 0.25, 0.5 + 0.04, 0.75 - 0.04, 1.0, 2.0, 2.25};
    auto x = syllables(local, 2.8, 50.0);
    std::vector<double> kicks = {4.0, 6.0, 8.0}, snares = {5.0, 7.0};
    auto r = analyzeFlow({x}, SR, start, tempo, kicks, snares);
    REQUIRE(r.onsets.size() == local.size());
    CHECK(r.onsets[0].placement == Placement::OnBeat);
    CHECK(r.onsets[2].placement == Placement::Late);
    CHECK_NEAR(r.onsets[2].offsetMs, 40.0, 8.0);
    CHECK(r.onsets[3].placement == Placement::Early);
    CHECK_NEAR(r.onsets[3].offsetMs, -40.0, 8.0);
    CHECK_NEAR(r.onsets[0].kickOffsetMs, 0.0, 8.0);
    CHECK_NEAR(r.onsets[4].beat, 6.0, 0.05);
    CHECK(r.phrases.size() == 2);
    CHECK(r.phrases[0].onsets == 5);
    CHECK(r.pauses.size() == 1);
    CHECK(!r.note.empty());
    // drum hits from a Beat Lab pattern
    Project p = makeNewProject("flow", 48000, 120);
    Pattern pat = makeDefaultPattern("P", 16);
    pat.rows[0].steps[0].on = true;  // kick on beat 0
    pat.rows[0].steps[8].on = true;  // kick on beat 2
    pat.rows[1].steps[4].on = true;  // snare on beat 1
    p.patterns.push_back(pat);
    const std::string bt = addTrack(p, TrackType::Beat, "Drums").id;
    p.findTrack(bt)->patternClips.push_back({files::newId(), pat.id, 0.0, 8.0, false, false, 0});
    auto k = drumHitsFromProject(p, "kick", 0, 8);
    CHECK(k == std::vector<double>({0.0, 2.0, 4.0, 6.0}));
    CHECK(drumHitsFromProject(p, "snare", 0, 8) == std::vector<double>({1.0, 5.0}));
}

TEST_CASE("gmb06", "onsets, bpm and key estimation") {
    // click track at 100 BPM with a C-major-ish chord pad
    const double bpm = 100.0;
    std::vector<float> x(static_cast<size_t>(SR * 12), 0.0f);
    for (double b = 0; b * 60.0 / bpm < 11.5; b += 1.0) {
        const size_t a = static_cast<size_t>(b * 60.0 / bpm * SR);
        for (size_t i = 0; i < 2400 && a + i < x.size(); ++i) x[a + i] += static_cast<float>(0.6 * std::exp(-static_cast<double>(i) / 400.0) * std::sin(kTwoPi * 150.0 * i / SR));
    }
    auto onsets = dsp::detectOnsets(x.data(), static_cast<int64_t>(x.size()), SR);
    CHECK(onsets.size() >= 18 && onsets.size() <= 21);
    auto t = dsp::estimateBpm(x.data(), static_cast<int64_t>(x.size()), SR);
    CHECK_NEAR(t.bpm, 100.0, 1.0);
    std::vector<float> chord(static_cast<size_t>(SR * 3));
    for (size_t i = 0; i < chord.size(); ++i)
        for (int m : {48, 52, 55, 60, 64, 67, 62, 65}) chord[i] += 0.05f * static_cast<float>(std::sin(kTwoPi * midiToHz(m) * i / SR));
    auto k = dsp::estimateKey(chord.data(), static_cast<int64_t>(chord.size()), SR);
    CHECK(k.root == 0);
    CHECK(!k.minor);
}
