// GMB 06 - Pitch detection, analysis and PITCH GUARDIAN / OFF-KEY FILTER.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "dsp/FFT.h"
#include "vocal/PitchAnalysis.h"
#include "vocal/PitchDetector.h"
#include "vocal/PitchGuardian.h"

using namespace roy;
using namespace roy::vocal;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

// median detected MIDI pitch in [t0, t1)
double medianPitch(const PitchTrack& t, double t0, double t1) {
    std::vector<double> v;
    for (auto& f : t.frames)
        if (f.voiced && f.time >= t0 && f.time < t1) v.push_back(f.midi);
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double centroid(const std::vector<float>& x, size_t from, double lo, double hi) {
    const int N = 8192;
    auto mag = dsp::magnitudeSpectrum(x.data() + from, N);
    double num = 0, den = 0;
    for (size_t i = 0; i < mag.size(); ++i) {
        const double f = static_cast<double>(i) * SR / N;
        if (f < lo || f > hi) continue;
        num += f * mag[i] * mag[i];
        den += mag[i] * mag[i];
    }
    return den > 0 ? num / den : 0;
}

PitchGuardianSettings cMajorAssist() {
    PitchGuardianSettings s;
    s.mode = GuardianMode::Assist;
    s.key = *parseKey("C Major");
    s.speedMs = 5;
    s.humanize = 1.0;
    return s;
}
} // namespace

TEST_CASE("gmb06", "pitch detection accuracy") {
    for (double hz : {98.0, 220.0, 440.0, 880.0}) {
        auto sine = makeSine(SR, hz, 0.5, 0.5f, 1);
        auto t = detectPitch(sine->channels[0].data(), sine->numFrames, SR);
        const double m = medianPitch(t, 0.05, 0.45);
        CHECK_MSG(std::fabs(m - hzToMidi(hz)) * 100 < 3.0, std::format("{} Hz -> {} midi", hz, m));
    }
    VoiceSpec v;
    v.midi = [](double) { return 57.3; };
    auto voice = makeVoice(v);
    auto t = detectPitch(voice.data(), static_cast<int64_t>(voice.size()), SR);
    CHECK_NEAR(medianPitch(t, 0.05, 0.95), 57.3, 0.03);
    // silence and white noise are not reported as confident pitch
    std::vector<float> silence(24000, 0.0f);
    auto ts = detectPitch(silence.data(), 24000, SR);
    for (auto& f : ts.frames) CHECK(!f.voiced);
    Rng rng(3);
    std::vector<float> noise(48000);
    for (auto& x : noise) x = static_cast<float>(rng.uniform(-0.3, 0.3));
    auto tn = detectPitch(noise.data(), 48000, SR);
    int voiced = 0;
    for (auto& f : tn.frames) voiced += f.voiced ? 1 : 0;
    CHECK(voiced < static_cast<int>(tn.frames.size()) / 10);
}

TEST_CASE("gmb06", "analysis distinguishes stable, vibrato, slide") {
    VoiceSpec v;
    v.seconds = 2.4;
    v.midi = [](double t) {
        if (t < 0.6) return 60.0;                                             // stable
        if (t < 0.7) return 0.0;                                              // gap
        if (t < 1.5) return 64.0 + 0.5 * std::sin(kTwoPi * 5.5 * (t - 0.7)); // vibrato +-50 cents
        if (t < 1.6) return 0.0;
        if (t < 1.9) return 60.0 + 5.0 * (t - 1.6) / 0.3;                     // slide up 5 semitones
        if (t < 2.3) return 65.0;
        return 0.0;
    };
    auto x = makeVoice(v);
    auto track = detectPitch(x.data(), static_cast<int64_t>(x.size()), SR);
    auto a = analyzePitch(track);
    bool stable = false, vib = false, slide = false;
    for (auto& n : a.notes) {
        if (n.kind == FrameKind::Stable && std::fabs(n.medianMidi - 60) < 0.1 && n.start < 0.1) stable = true;
        if (n.kind == FrameKind::Vibrato) {
            vib = true;
            CHECK_NEAR(n.vibratoRateHz, 5.5, 1.0);
            CHECK_NEAR(n.vibratoExtentCents, 50.0, 15.0);
            CHECK_NEAR(n.medianMidi, 64.0, 0.15);
        }
        if (n.kind == FrameKind::Slide) {
            slide = true;
            CHECK(n.slideSemitones > 2.0);
        }
    }
    CHECK(stable);
    CHECK(vib);
    CHECK(slide);
    CHECK(a.voicedSeconds > 1.8);
}

TEST_CASE("gmb06", "ASSIST corrects a sharp note and leaves in-tune notes untouched") {
    VoiceSpec v;
    v.seconds = 2.0;
    v.midi = [](double t) {
        if (t < 0.8) return 60.42; // 42 cents sharp
        if (t < 1.0) return 0.0;
        return 62.10;              // 10 cents: within threshold
    };
    auto x = makeVoice(v);
    auto s = cMajorAssist();
    auto r = runPitchGuardian({x}, SR, s);
    REQUIRE(r.plan.any());
    auto after = detectPitch(r.audio[0].data(), static_cast<int64_t>(r.audio[0].size()), SR);
    CHECK_NEAR(medianPitch(after, 0.15, 0.7), 60.0, 0.06);
    CHECK_NEAR(medianPitch(after, 1.15, 1.9), 62.10, 0.03);
    // the in-tune note is bit-identical to the original (no processing where not needed)
    double maxDiff = 0;
    for (size_t i = static_cast<size_t>(1.2 * SR); i < static_cast<size_t>(1.9 * SR); ++i)
        maxDiff = std::max(maxDiff, std::fabs(double(r.audio[0][i]) - x[i]));
    CHECK(maxDiff == 0.0);
    CHECK(allFinite(r.audio[0]));
    // the original buffer was not modified
    auto again = makeVoice(v);
    CHECK(again == x);
}

TEST_CASE("gmb06", "vibrato is preserved while the centre is corrected") {
    VoiceSpec v;
    v.seconds = 1.5;
    v.midi = [](double t) { return 64.35 + 0.4 * std::sin(kTwoPi * 5.0 * t); };
    auto x = makeVoice(v);
    auto s = cMajorAssist();
    s.vibratoPreserve = 1.0;
    auto r = runPitchGuardian({x}, SR, s);
    auto a = analyzePitch(detectPitch(r.audio[0].data(), static_cast<int64_t>(r.audio[0].size()), SR));
    bool found = false;
    for (auto& n : a.notes)
        if (n.kind == FrameKind::Vibrato) {
            found = true;
            CHECK_NEAR(n.medianMidi, 64.0, 0.08);
            CHECK_NEAR(n.vibratoExtentCents, 40.0, 12.0);
        }
    CHECK(found);
    // vibratoPreserve = 0 flattens the vibrato
    s.vibratoPreserve = 0.0;
    auto flat = runPitchGuardian({x}, SR, s);
    auto a2 = analyzePitch(detectPitch(flat.audio[0].data(), static_cast<int64_t>(flat.audio[0].size()), SR));
    for (auto& n : a2.notes) CHECK(n.kind != FrameKind::Vibrato || n.vibratoExtentCents < 20.0);
}

TEST_CASE("gmb06", "OFF-KEY filter and ALLOW CHROMATIC decide targets") {
    VoiceSpec v;
    v.seconds = 0.8;
    v.midi = [](double) { return 61.3; }; // C# + 30 cents; C# is not in C major
    auto x = makeVoice(v);
    auto track = detectPitch(x.data(), static_cast<int64_t>(x.size()), SR);
    auto a = analyzePitch(track);
    auto noteTarget = [&](const PitchGuardianSettings& s) {
        auto plan = planCorrection(track, a, s);
        for (auto& n : plan.notes)
            if (n.corrected) return n.toMidi;
        return -1.0;
    };
    auto s = cMajorAssist();
    s.offKeyFilter = true;
    s.allowChromatic = false;
    CHECK_NEAR(noteTarget(s), 62.0, 1e-9); // never pulled to the off-key C#: goes to D
    s.allowChromatic = true;
    CHECK_NEAR(noteTarget(s), 61.0, 1e-9); // artist intends C#: kept chromatic
    s.offKeyFilter = false;
    s.allowChromatic = false;
    CHECK_NEAR(noteTarget(s), 61.0, 1e-9); // filter off: nearest semitone
    s.mode = GuardianMode::Lock;
    CHECK_NEAR(noteTarget(s), 62.0, 1e-9); // LOCK binds to the scale
    s.allowChromatic = true;
    s.chromaticToleranceCents = 40.0;
    CHECK_NEAR(noteTarget(s), 61.0, 1e-9); // within chromatic tolerance
    // warnings mention the off-key note
    s.mode = GuardianMode::Warn;
    auto plan = planCorrection(track, a, s);
    CHECK(!plan.any());
    REQUIRE(!plan.warnings.empty());
    CHECK(plan.warnings[0].offKey);
    s.mode = GuardianMode::Off;
    CHECK(!planCorrection(track, a, s).any());
}

TEST_CASE("gmb06", "low confidence is not corrected aggressively") {
    VoiceSpec v;
    v.seconds = 0.8;
    v.midi = [](double) { return 60.45; };
    v.noise = 0.25; // very noisy recording
    auto x = makeVoice(v);
    auto track = detectPitch(x.data(), static_cast<int64_t>(x.size()), SR);
    auto a = analyzePitch(track);
    auto s = cMajorAssist();
    s.minConfidence = 0.95;
    auto plan = planCorrection(track, a, s);
    double maxShift = 0;
    for (double d : plan.shift) maxShift = std::max(maxShift, std::fabs(d));
    // with a clean signal the full 0.45 st correction would be applied
    CHECK(maxShift < 0.3);
}

TEST_CASE("gmb06", "formant preservation") {
    VoiceSpec v;
    v.seconds = 1.0;
    v.midi = [](double) { return 55.0; };
    auto x = makeVoice(v);
    auto track = detectPitch(x.data(), static_cast<int64_t>(x.size()), SR);
    std::vector<double> shift(track.frames.size(), 4.0); // +4 semitones everywhere
    auto keep = psolaShift({x}, SR, track, shift, true);
    auto move = psolaShift({x}, SR, track, shift, false);
    auto pk = detectPitch(keep[0].data(), static_cast<int64_t>(keep[0].size()), SR);
    auto pm = detectPitch(move[0].data(), static_cast<int64_t>(move[0].size()), SR);
    CHECK_NEAR(medianPitch(pk, 0.2, 0.8), 59.0, 0.1);
    CHECK_NEAR(medianPitch(pm, 0.2, 0.8), 59.0, 0.1);
    const double c0 = centroid(x, 12000, 300, 1600);
    const double ck = centroid(keep[0], 12000, 300, 1600);
    const double cm = centroid(move[0], 12000, 300, 1600);
    CHECK_MSG(std::fabs(ck / c0 - 1.0) < 0.08, std::format("preserved centroid {} vs {}", ck, c0));
    CHECK_MSG(cm / c0 > 1.12, std::format("moved centroid {} vs {}", cm, c0));
}

TEST_CASE("gmb06", "slide is preserved and transitions stay continuous") {
    VoiceSpec v;
    v.seconds = 1.4;
    v.midi = [](double t) {
        if (t < 0.5) return 57.35;
        if (t < 0.8) return 57.35 + 4.7 * (t - 0.5) / 0.3; // slide to ~62.05
        return 62.05;
    };
    auto x = makeVoice(v);
    auto s = cMajorAssist();
    s.slidePreserve = 1.0;
    auto r = runPitchGuardian({x}, SR, s);
    // shift curve has no jumps larger than 0.2 st between frames
    double maxJump = 0;
    for (size_t i = 1; i < r.plan.shift.size(); ++i) maxJump = std::max(maxJump, std::fabs(r.plan.shift[i] - r.plan.shift[i - 1]));
    CHECK(maxJump < 0.2);
    auto after = detectPitch(r.audio[0].data(), static_cast<int64_t>(r.audio[0].size()), SR);
    CHECK_NEAR(medianPitch(after, 0.1, 0.45), 57.0, 0.08);
    // the slide still moves smoothly upwards
    CHECK(medianPitch(after, 0.7, 0.75) - medianPitch(after, 0.55, 0.6) > 1.5);
}
