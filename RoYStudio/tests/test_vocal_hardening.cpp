// Vocal hardening: Pitch Guardian / OFF-KEY filter on more demanding synthetic material
// (MOCK voices with jitter, shimmer, breathiness, consonants, breaths, background noise).
// Real recordings remain UNTESTED here; see 00_IMPORTANT/TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md.
#include "TestFramework.h"
#include "VocalMaterial.h"

#include "vocal/PitchAnalysis.h"
#include "vocal/PitchDetector.h"
#include "vocal/PitchGuardian.h"

#include <algorithm>

using namespace roy;
using namespace roy::vocal;
using namespace roytest;

namespace {
double medianMidi(const PitchTrack& t, double t0, double t1) {
    std::vector<double> v;
    for (auto& f : t.frames)
        if (f.voiced && f.time >= t0 && f.time < t1) v.push_back(f.midi);
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double noteMedian(const std::vector<float>& audio, double sr, const TruthNote& n) {
    const auto t = detectPitch(audio.data(), static_cast<int64_t>(audio.size()), sr);
    const double margin = std::min(0.08, (n.end - n.start) * 0.25);
    return medianMidi(t, n.start + margin, n.end - margin);
}

PitchGuardianSettings settings(GuardianMode m, Key k) {
    PitchGuardianSettings s;
    s.mode = m;
    s.key = k;
    s.speedMs = 5.0;
    s.strength = 1.0;
    return s;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
    double m = 0;
    for (size_t i = from; i < std::min({to, a.size(), b.size()}); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
} // namespace

TEST_CASE("vocal", "detection across the vocal range with jitter, shimmer, breathiness and noise") {
    struct Case { double midi; double noiseDb; };
    for (Case c : {Case{43, -90}, Case{45, -60}, Case{55, -90}, Case{64, -45}, Case{72, -60}, Case{79, -90}, Case{84, -60}}) {
        VocalSpec v;
        v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.8, c.midi, 0, 0, 0, 0.3}, {SegKind::Rest, 0.1}};
        v.noiseDb = c.noiseDb;
        const auto take = makeVocal(v);
        const double m = noteMedian(take.audio, take.sr, take.truth[0]);
        CHECK_MSG(std::fabs(m - c.midi) < 0.12, std::format("midi {} noise {} dB -> detected {:.3f}", c.midi, c.noiseDb, m));
    }
}

TEST_CASE("vocal", "ASSIST corrects detuned notes to the nearest in-key note on realistic material") {
    VocalSpec v;
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.5, 60.35}, {SegKind::Breath, 0.15, 0, 0, 0, 0, 0.2}, {SegKind::Note, 0.5, 63.7},
              {SegKind::Sibilant, 0.08, 0, 0, 0, 0, 0.2}, {SegKind::Note, 0.5, 67.1}, {SegKind::Rest, 0.1}};
    v.noiseDb = -60;
    const auto take = makeVocal(v);
    Key cmaj{0, ScaleType::Major};
    const auto r = runPitchGuardian({take.audio}, take.sr, settings(GuardianMode::Assist, cmaj));
    const double expect[] = {60, 64, 67.1}; // 10 cents is inside the 25-cent ASSIST threshold: left natural
    for (size_t i = 0; i < take.truth.size(); ++i) {
        const double m = noteMedian(r.audio[0], take.sr, take.truth[i]);
        CHECK_MSG(std::fabs(m - expect[i]) < 0.08, std::format("note {}: {:.2f} -> {:.3f} (want {})", i, take.truth[i].midi, m, expect[i]));
    }
}

TEST_CASE("vocal", "breaths, sibilants and plosives pass through untouched") {
    VocalSpec v;
    v.segs = {{SegKind::Note, 0.4, 60.4}, {SegKind::Breath, 0.3, 0, 0, 0, 0, 0.25}, {SegKind::Sibilant, 0.15, 0, 0, 0, 0, 0.25},
              {SegKind::Plosive, 0.1, 0, 0, 0, 0, 0.3}, {SegKind::Rest, 0.1}, {SegKind::Note, 0.4, 64.4}};
    const auto take = makeVocal(v);
    const auto r = runPitchGuardian({take.audio}, take.sr, settings(GuardianMode::Lock, Key{0, ScaleType::Major}));
    REQUIRE(r.plan.any());
    // the unvoiced middle (breath + sibilant + plosive), away from note edges, is bit-identical
    const size_t a = static_cast<size_t>((0.4 + 0.08) * take.sr), b = static_cast<size_t>((0.4 + 0.55 - 0.05) * take.sr);
    CHECK_MSG(maxAbsDiff(take.audio, r.audio[0], a, b) == 0.0, std::format("max diff {}", maxAbsDiff(take.audio, r.audio[0], a, b)));
}

TEST_CASE("vocal", "fast note changes are corrected note by note (no smearing)") {
    VocalSpec v;
    const double notes[] = {60.3, 62.4, 64.3, 65.35, 67.3, 69.4, 71.3, 72.35};
    v.segs.push_back({SegKind::Rest, 0.1});
    for (double m : notes) v.segs.push_back({SegKind::Note, 0.14, m, 0, 0, 0, 0.3}); // ~7 notes/s, legato
    v.segs.push_back({SegKind::Rest, 0.1});
    const auto take = makeVocal(v);
    auto s = settings(GuardianMode::Lock, Key{0, ScaleType::Major});
    s.speedMs = 0;
    const auto r = runPitchGuardian({take.audio}, take.sr, s);
    int good = 0;
    for (size_t i = 0; i < take.truth.size(); ++i) {
        const double want = std::round(take.truth[i].midi);
        const double m = noteMedian(r.audio[0], take.sr, take.truth[i]);
        if (std::fabs(m - want) < 0.15) ++good;
        else std::printf("       fast note %zu: %.2f -> %.3f (want %.0f)\n", i, take.truth[i].midi, m, want);
    }
    CHECK_MSG(good >= 7, std::format("{}/8 fast notes on target", good));
}

TEST_CASE("vocal", "OFF-KEY filter in Major, Minor, Dorian, Phrygian, Pentatonic; ALLOW CHROMATIC") {
    struct Mode { ScaleType t; const char* name; };
    for (Mode md : {Mode{ScaleType::Major, "major"}, Mode{ScaleType::NaturalMinor, "minor"}, Mode{ScaleType::Dorian, "dorian"},
                    Mode{ScaleType::Phrygian, "phrygian"}, Mode{ScaleType::MinorPentatonic, "minor pentatonic"},
                    Mode{ScaleType::MajorPentatonic, "major pentatonic"}}) {
        const Key key{9, md.t}; // root A
        VocalSpec v;
        v.segs.push_back({SegKind::Rest, 0.1});
        for (int pc = 0; pc < 12; ++pc) {
            v.segs.push_back({SegKind::Note, 0.3, 57.0 + pc + 0.2, 0, 0, 0, 0.3}); // every chromatic note, 20 cents sharp
            v.segs.push_back({SegKind::Rest, 0.08});
        }
        const auto take = makeVocal(v);
        auto s = settings(GuardianMode::Lock, key);
        s.offKeyFilter = true;
        const auto r = runPitchGuardian({take.audio}, take.sr, s);
        int inKey = 0, total = 0;
        for (auto& n : take.truth) {
            const double m = noteMedian(r.audio[0], take.sr, n);
            ++total;
            const int nearest = static_cast<int>(std::lround(m));
            if (std::fabs(m - nearest) < 0.1 && key.contains(nearest)) ++inKey;
            else std::printf("       %s: sung %.2f -> %.3f\n", md.name, n.midi, m);
        }
        CHECK_MSG(inKey == total, std::format("{}: {}/{} notes land on scale notes", md.name, inKey, total));
    }
    // ALLOW CHROMATIC: a clearly sung out-of-key note (F# in C major, 10 cents) stays chromatic
    VocalSpec v;
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.5, 66.1}, {SegKind::Rest, 0.1}};
    const auto take = makeVocal(v);
    auto s = settings(GuardianMode::Assist, Key{0, ScaleType::Major});
    s.offKeyFilter = true;
    s.allowChromatic = true;
    const auto keep = runPitchGuardian({take.audio}, take.sr, s);
    CHECK_NEAR(noteMedian(keep.audio[0], take.sr, take.truth[0]), 66.0, 0.1);
    s.allowChromatic = false;
    s.mode = GuardianMode::Lock;
    const auto snap = runPitchGuardian({take.audio}, take.sr, s);
    const double m = noteMedian(snap.audio[0], take.sr, take.truth[0]);
    CHECK_MSG(std::fabs(m - 65.0) < 0.1 || std::fabs(m - 67.0) < 0.1, std::format("{:.3f}", m));
}

TEST_CASE("vocal", "rap: speech-like glides are not pulled to notes") {
    const auto take = makeVocal(rapSpec());
    const auto r = runPitchGuardian({take.audio}, take.sr, settings(GuardianMode::Assist, Key{0, ScaleType::Major}));
    CHECK_MSG(r.analysis.rapIndicator > 0.7, std::format("rap indicator {:.2f}", r.analysis.rapIndicator));
    int corrected = 0;
    for (auto& n : r.plan.notes) corrected += n.corrected ? 1 : 0;
    double maxShift = 0;
    for (double sh : r.plan.shift) maxShift = std::max(maxShift, std::fabs(sh));
    CHECK_MSG(corrected <= static_cast<int>(r.plan.notes.size() / 5), std::format("{} of {} rap segments corrected", corrected, r.plan.notes.size()));
    std::printf("       rap: %d/%zu segments corrected, max shift %.2f st\n", corrected, r.plan.notes.size(), maxShift);
}

TEST_CASE("vocal", "low confidence (heavy noise) is never corrected aggressively") {
    VocalSpec v;
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 0.8, 60.45, 0, 0, 0, 0.05}, {SegKind::Rest, 0.1}};
    v.noiseDb = -24; // noise almost as loud as the voice
    const auto take = makeVocal(v);
    const auto r = runPitchGuardian({take.audio}, take.sr, settings(GuardianMode::Lock, Key{0, ScaleType::Major}));
    double maxShift = 0;
    for (double sh : r.plan.shift) maxShift = std::max(maxShift, std::fabs(sh));
    // at most the correct half-semitone pull, never a big jump to a wrong note
    CHECK_MSG(maxShift < 0.6, std::format("max shift {:.2f}", maxShift));
}

TEST_CASE("vocal", "vibrato and slides preserved on a high voice") {
    VocalSpec v;
    v.segs = {{SegKind::Rest, 0.1}, {SegKind::Note, 1.0, 76.3, 0, 60, 5.5, 0.3}, {SegKind::Slide, 0.4, 76.0, 79.0, 0, 0, 0.3},
              {SegKind::Note, 0.6, 79.25, 0, 0, 0, 0.3}, {SegKind::Rest, 0.1}};
    const auto take = makeVocal(v);
    const auto r = runPitchGuardian({take.audio}, take.sr, settings(GuardianMode::Assist, Key{0, ScaleType::Major}));
    const auto out = detectPitch(r.audio[0].data(), static_cast<int64_t>(r.audio[0].size()), take.sr);
    // vibrato centre corrected to 76, depth kept (peak-to-peak ~ 120 cents)
    std::vector<double> vib, vin;
    for (auto& f : out.frames)
        if (f.voiced && f.time > 0.4 && f.time < 1.0) vib.push_back(f.midi);
    for (auto& f : r.track.frames)
        if (f.voiced && f.time > 0.4 && f.time < 1.0) vin.push_back(f.midi);
    std::sort(vin.begin(), vin.end());

    REQUIRE(vib.size() > 20);
    std::sort(vib.begin(), vib.end());
    const double p2p = vib[vib.size() * 95 / 100] - vib[vib.size() * 5 / 100];
    CHECK_MSG(p2p > 0.8 && p2p < 1.6, std::format("vibrato p2p {:.2f} st", p2p));
    // centre moved by exactly the planned note shift (note median -> E5); natural drift inside the note is kept
    REQUIRE(!r.plan.notes.empty());
    CHECK_NEAR(r.plan.notes[0].toMidi, 76.0, 1e-9);
    CHECK_NEAR(vib[vib.size() / 2] - vin[vin.size() / 2], r.plan.notes[0].shift, 0.03);
    // slide keeps moving (not quantised to a step)
    const double mid = medianMidi(out, 1.25, 1.35);
    CHECK_MSG(mid > 76.8 && mid < 78.2, std::format("slide middle {:.2f}", mid));
}

