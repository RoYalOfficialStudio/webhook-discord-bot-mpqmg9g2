// GMB 13.1 - 13.4: realtime pitch tracker, PSOLA shifter and RoY VocalTune 2 (roy.vocaltune2).
// Automated measurements only - sound quality needs a listening test.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "DiagSignals.h"

#include "commands/Commands.h"
#include "core/Files.h"
#include "diag/ClickDetector.h"
#include "dsp/FFT.h"
#include "dsp/PitchTrackerRt.h"
#include "dsp/PsolaShifter.h"
#include "effects/Special.h"
#include "effects/VocalTune2.h"
#include "vocal/PitchDetector.h"

#include <format>

using namespace roy;
using namespace roytest;
using namespace roytest::gmb13;

namespace {
// dominant frequency with parabolic interpolation (log magnitude)
double dominantHz(const std::vector<float>& y, size_t from, double sr = SR, int N = 32768) {
    REQUIRE(y.size() >= from + static_cast<size_t>(N));
    auto mag = dsp::magnitudeSpectrum(y.data() + from, N);
    size_t b = 2;
    for (size_t i = 3; i + 1 < mag.size(); ++i)
        if (mag[i] > mag[b]) b = i;
    const double a = std::log(mag[b - 1] + 1e-12), c = std::log(mag[b] + 1e-12), d = std::log(mag[b + 1] + 1e-12);
    const double off = 0.5 * (a - d) / (a - 2 * c + d);
    return (static_cast<double>(b) + off) * sr / N;
}
double cents(double hz, double refHz) { return 1200.0 * std::log2(hz / refHz); }

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

struct Vt2 {
    RoyVocalTune2 vt;
    explicit Vt2(double sr = SR, float speed = 20, int root = 0, int scale = 1) {
        vt.setParam("keyRoot", static_cast<float>(root));
        vt.setParam("keyScale", static_cast<float>(scale));
        vt.setParam("speed", speed);
        vt.prepare(sr, 1024);
    }
};

double medianMidi(const std::vector<float>& y, double sr, double t0, double t1) {
    auto tr = vocal::detectPitch(y.data(), static_cast<int64_t>(y.size()), sr);
    std::vector<double> v;
    for (auto& f : tr.frames)
        if (f.voiced && f.time >= t0 && f.time < t1) v.push_back(f.midi);
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

// ---------------------------------------------------------------------------------- 13.1 tracker
TEST_CASE("vocaltune2", "realtime tracker: accuracy 80..900 Hz at 44.1/48 kHz, confidence, silence, noise, no allocation") {
    for (double sr : {44100.0, 48000.0}) {
        for (double hz : {82.0, 110.0, 220.0, 440.0, 660.0, 880.0}) {
            dsp::PitchTrackerRt t;
            t.prepare(sr);
            const auto x = faded(tone(hz, 1.0, 0.3f, sr));
            for (float v : x) t.push(v);
            const double err = (t.midi() - hzToMidi(hz)) * 100.0;
            CHECK_MSG(t.voiced() && std::fabs(err) < 5.0, std::format("{} Hz @ {}: voiced {} err {:.2f} c conf {:.2f}", hz, sr, t.voiced(), err, t.confidence()));
        }
    }
    {
        dsp::PitchTrackerRt t; // voice with vibrato
        t.prepare(SR);
        for (float v : faded(smoothVoice([](double) { return 57.4; }, 1.0))) t.push(v);
        CHECK(t.voiced());
        CHECK_NEAR(t.midi(), 57.4, 0.05);
        CHECK(t.confidence() > 0.8);
    }
    {
        dsp::PitchTrackerRt t; // silence and white noise: unvoiced
        t.prepare(SR);
        for (int i = 0; i < 48000; ++i) t.push(0.0f);
        CHECK(!t.voiced());
        Rng rng(3);
        int voicedEstimates = 0, estimates = 0;
        for (int i = 0; i < 96000; ++i)
            if (t.push(static_cast<float>(rng.uniform(-0.3, 0.3)))) {
                ++estimates;
                voicedEstimates += t.voiced();
            }
        std::printf("    white noise: %d of %d estimates voiced\n", voicedEstimates, estimates);
        CHECK(estimates > 400);
        CHECK(voicedEstimates <= estimates / 50);
    }
    {
        dsp::PitchTrackerRt t;
        t.prepare(SR);
        const auto x = tone(220, 1.0, 0.3f);
        AllocationCounter c;
        for (float v : x) t.push(v);
        CHECK(c.count() == 0);
    }
}

// ---------------------------------------------------------------------------------- 13.2 PSOLA
TEST_CASE("vocaltune2", "PSOLA: fixed latency, transparent at ratio 1, exact pitch shift, formants kept") {
    dsp::PsolaShifter ps;
    ps.prepare(SR, 1, 80.0);
    const int L = ps.latencySamples();
    std::printf("    PSOLA latency at 48 kHz: %d samples (%.2f ms)\n", L, L * 1000.0 / SR);
    CHECK(L == 2 * 600 + 64);
    // impulse, unvoiced -> comes out exactly L samples later
    {
        ps.reset();
        std::vector<float> out(9600);
        for (int i = 0; i < 9600; ++i) {
            const float in = i == 1000 ? 1.0f : 0.0f;
            float w, d;
            ps.process(&in, &w, &d, 0.0, 1.0, 1.0);
            out[static_cast<size_t>(i)] = w;
        }
        CHECK(argmaxAbs(out) == static_cast<size_t>(1000 + L));
        CHECK_NEAR(out[static_cast<size_t>(1000 + L)], 1.0f, 1e-5f);
    }
    // voiced sine, ratio 1: delayed input
    {
        ps.reset();
        const auto x = faded(tone(220, 1.0, 0.4f));
        double maxErr = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            float w, d;
            ps.process(&x[i], &w, &d, SR / 220.0, 1.0, 1.0);
            if (i >= static_cast<size_t>(L)) maxErr = std::max(maxErr, static_cast<double>(std::fabs(w - x[i - static_cast<size_t>(L)])));
        }
        std::printf("    ratio 1, voiced sine: max deviation from the delayed input %.2e\n", maxErr);
        CHECK(maxErr < 1e-4);
    }
    // +-5 semitones on a voice: pitch exact, formant centroid kept (formant 1) / moved (formant = ratio)
    for (double st : {-5.0, 2.0, 5.0}) {
        const double ratio = std::pow(2.0, st / 12.0);
        VoiceSpec vs; // constant pitch: the harmonic set does not change, formant envelope in Hz
        vs.seconds = 1.2;
        vs.midi = [](double) { return 55.0; };
        auto x = faded(makeVoice(vs));
        for (int mode = 0; mode < 2; ++mode) {
            dsp::PsolaShifter p2;
            p2.prepare(SR, 1, 80.0);
            std::vector<float> y(x.size());
            for (size_t i = 0; i < x.size(); ++i) {
                float d;
                p2.process(&x[i], &y[i], &d, SR / midiToHz(55.0), ratio, mode == 0 ? 1.0 : ratio);
            }
            const double m = medianMidi(y, SR, 0.3, 1.1);
            const double c0 = centroid(x, 24000, 300, 1600), c1 = centroid(y, 24000, 300, 1600);
            std::printf("    %+.0f st, formant %s: pitch %.3f (target %.3f), F-centroid %.0f -> %.0f Hz (%+.1f %%)\n", st, mode == 0 ? "kept " : "moved",
                        m, 55.0 + st, c0, c1, 100.0 * (c1 / c0 - 1));
            CHECK_NEAR(m, 55.0 + st, 0.06);
            if (mode == 0) CHECK_MSG(std::fabs(c1 / c0 - 1.0) < 0.10, std::format("{} st: formants moved {:.1f} %", st, 100 * (c1 / c0 - 1)));
            else CHECK(std::fabs(c1 / c0 - 1.0) > 0.08);
            CHECK(allFinite(y));
        }
    }
    {
        AllocationCounter c;
        float in = 0.1f, w, d;
        for (int i = 0; i < 48000; ++i) ps.process(&in, &w, &d, 200.0, 1.1, 1.0);
        CHECK(c.count() == 0);
    }
}

// ---------------------------------------------------------------------------------- 13.3 / 13.4 VocalTune 2
TEST_CASE("vocaltune2", "VocalTune 2 corrects to the scale: +-40 cents at several pitches, 44.1 and 48 kHz") {
    for (double sr : {44100.0, 48000.0}) {
        for (double note : {45.0, 57.0, 64.0, 69.0, 76.0}) { // A2 A3 E4 A4 E5 (all in C major)
            for (double off : {+0.4, -0.4}) {
                Vt2 v(sr, 20);
                const auto x = faded(tone(midiToHz(note + off), 2.0, 0.3f, sr));
                const auto y = runMono(v.vt, x, {256});
                const double c = cents(dominantHz(y, static_cast<size_t>(0.9 * sr), sr), midiToHz(note));
                CHECK_MSG(std::fabs(c) < 5.0, std::format("{:.0f} Hz: note {} {:+.1f}: result {:+.2f} cents", sr, note, off, c));
            }
        }
    }
}

TEST_CASE("vocaltune2", "VocalTune 2: chromatic vs diatonic targets, threshold, unvoiced/noise do not move the pitch") {
    // F#4 (66) in C major: off-key filter -> nearest scale note (F or G); allow chromatic -> stays F#
    for (int chromatic = 0; chromatic < 2; ++chromatic) {
        Vt2 v(SR, 0);
        v.vt.setParam("allowChromatic", static_cast<float>(chromatic));
        const auto x = faded(tone(midiToHz(66.2), 2.0, 0.3f));
        const auto y = runMono(v.vt, x, {256});
        const double m = hzToMidi(dominantHz(y, 48000));
        std::printf("    F#4+20c, allowChromatic %d -> %.2f\n", chromatic, m);
        if (chromatic) CHECK_NEAR(m, 66.0, 0.05);
        else CHECK(std::fabs(m - 65.0) < 0.05 || std::fabs(m - 67.0) < 0.05);
    }
    { // within threshold (15 cents): left natural
        Vt2 v(SR, 0);
        const auto x = faded(tone(midiToHz(64.1), 2.0, 0.3f));
        const auto y = runMono(v.vt, x, {256});
        CHECK_NEAR(hzToMidi(dominantHz(y, 48000)), 64.1, 0.03);
    }
    { // white noise: no correction, no jumps
        Vt2 v(SR, 0);
        Rng rng(5);
        std::vector<float> x(96000);
        for (auto& s : x) s = static_cast<float>(rng.uniform(-0.2, 0.2));
        double maxShift = 0;
        std::vector<float> l = x, r = x;
        for (size_t off = 0; off < x.size(); off += 256) {
            float* ptr[2] = {l.data() + off, r.data() + off};
            AudioBlock b{ptr, 2, 256};
            v.vt.process(b, nullptr, nullptr, 0);
            maxShift = std::max(maxShift, std::fabs(v.vt.appliedShift()));
        }
        std::printf("    white noise: max applied shift %.3f st\n", maxShift);
        CHECK(maxShift < 1.0);
        CHECK(allFinite(l));
    }
}

TEST_CASE("vocaltune2", "VocalTune 2: vibrato kept (vibrato 1) or flattened (Hard), held notes, glides without steps") {
    auto vib = faded(smoothVoice([](double t) { return 57.3 + 0.5 * std::sin(2 * std::numbers::pi * 5.5 * t); }, 3.0));
    auto depth = [&](const std::vector<float>& y, double& centre) {
        auto tr = vocal::detectPitch(y.data(), static_cast<int64_t>(y.size()), SR);
        double lo = 1e9, hi = -1e9, sum = 0;
        int n = 0;
        for (auto& f : tr.frames)
            if (f.voiced && f.time > 1.0 && f.time < 2.8) {
                lo = std::min(lo, f.midi);
                hi = std::max(hi, f.midi);
                sum += f.midi;
                ++n;
            }
        centre = n ? sum / n : 0;
        return (hi - lo) / 2;
    };
    double c0 = 0, c1 = 0, c2 = 0;
    const double d0 = depth(vib, c0);
    Vt2 keep(SR, 20);
    keep.vt.setParam("vibrato", 1.0f);
    const auto yk = runMono(keep.vt, vib, {256});
    const double d1 = depth(yk, c1);
    Vt2 hard(SR, 0);
    hard.vt.setParam("mode", static_cast<float>(RoyVocalTune2::Hard));
    const auto yh = runMono(hard.vt, vib, {256});
    const double d2 = depth(yh, c2);
    std::printf("    vibrato depth input %.2f st (centre %.2f) | keep: %.2f st (centre %.2f) | hard: %.2f st (centre %.2f)\n", d0, c0, d1, c1, d2, c2);
    CHECK(d1 >= 0.8 * d0);
    CHECK_NEAR(c1, 57.0, 0.08);
    CHECK(d2 < 0.5 * d0);
    // humanize on a held, slightly sharp note: gentler correction
    double hm[2];
    for (int h = 0; h < 2; ++h) {
        Vt2 v(SR, 20);
        v.vt.setParam("humanize", h ? 1.0f : 0.0f);
        const auto y = runMono(v.vt, faded(tone(midiToHz(60.35), 2.0, 0.3f)), {256});
        hm[h] = hzToMidi(dominantHz(y, 48000));
    }
    std::printf("    held C4+35c: humanize 0 -> %.3f, humanize 1 -> %.3f\n", hm[0], hm[1]);
    CHECK_NEAR(hm[0], 60.0, 0.05);
    CHECK(hm[1] > hm[0] + 0.1);
    // glide: no clicks in the output
    Vt2 g(SR, 0);
    const auto yg = runMono(g.vt, faded(glide(220, 440, 3, 0.3f)), {256});
    const auto rep = diag::detectClicks(yg);
    std::printf("    glissando A3->A4 (hard): %zu clicks, max score %.1f\n", rep.count(), rep.maxScore);
    CHECK(rep.count() == 0);
}

TEST_CASE("vocaltune2", "VocalTune 2: dry/wet 0/50/100 %, no comb filter, smooth parameter changes, no clicks") {
    const int L = [] {
        RoyVocalTune2 v;
        v.prepare(SR, 256);
        return v.latencySamples();
    }();
    const auto x = faded(tone(midiToHz(60.0), 2.0, 0.3f)); // in tune: wet == dry, so 50 % must not lose level
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        Vt2 v(SR, 20);
        v.vt.setParam("mix", mix);
        v.vt.reset();
        const auto y = runMono(v.vt, x, {256});
        if (mix == 0.0f) {
            double e = 0;
            for (size_t i = static_cast<size_t>(L); i < y.size(); ++i) e = std::max(e, static_cast<double>(std::fabs(y[i] - x[i - static_cast<size_t>(L)])));
            std::printf("    mix 0: max deviation from the input delayed by %d samples: %.2e\n", L, e);
            CHECK(e < 1e-6);
        } else {
            const double lvl = 20 * std::log10(rms(y, 48000) / rms(x, 48000));
            std::printf("    mix %.0f %%: level %+.2f dB vs input\n", mix * 100, lvl);
            CHECK(std::fabs(lvl) < 0.5); // a comb filter (misaligned dry path) would lose level here
        }
    }
    const auto mel = faded(melody());
    struct Change {
        const char* param;
        float from, to;
    };
    for (auto ch : {Change{"speed", 200, 0}, Change{"mix", 0, 1}, Change{"keyRoot", 0, 6}, Change{"mode", 0, 3}, Change{"formantShift", 0, 4},
                    Change{"strength", 0, 1}}) {
        Vt2 v(SR, 20);
        v.vt.setParam(ch.param, ch.from);
        v.vt.reset();
        std::vector<float> l = mel, r = mel;
        for (size_t off = 0; off < mel.size(); off += 256) {
            if (off == 48000 + 256 * 37) v.vt.setParam(ch.param, ch.to);
            const int k = static_cast<int>(std::min<size_t>(256, mel.size() - off));
            float* ptr[2] = {l.data() + off, r.data() + off};
            AudioBlock b{ptr, 2, k};
            v.vt.process(b, nullptr, nullptr, 0);
        }
        const auto rep = diag::detectClicks(l);
        std::printf("    %-12s %5.0f -> %-5.0f clicks %zu, max score %.1f\n", ch.param, ch.from, ch.to, rep.count(), rep.maxScore);
        CHECK(rep.count() == 0);
        CHECK(allFinite(l));
    }
}

TEST_CASE("vocaltune2", "VocalTune 2: block independence, silence, overload recovery, no allocation, old vs new") {
    registerBuiltinProcessors();
    const auto x = faded(melody());
    std::vector<float> ref;
    for (auto& bp : blockPatterns()) {
        Vt2 v(SR, 20);
        const auto y = runMono(v.vt, x, bp);
        if (ref.empty()) {
            ref = y;
            continue;
        }
        const double d = diag::maxAbsDifference(ref, y);
        std::printf("    block %-9s max diff to block 64: %.3g\n", patternName(bp).c_str(), d);
        CHECK(d <= 1e-6);
    }
    {
        Vt2 v;
        const auto y = runMono(v.vt, std::vector<float>(48000, 0.0f), {256});
        CHECK(peak(y) == 0.0f);
    }
    {
        Vt2 v;
        auto o = faded(tone(midiToHz(60.4), 2.0, 0.3f));
        for (size_t i = 0; i < 48000; ++i) o[i] *= 10.0f;
        const auto y = runMono(v.vt, o, {256});
        CHECK(allFinite(y));
        std::printf("    +20 dB for 1 s then normal: peak after 1.3 s %.3f\n", peak(y, 62400));
        CHECK(peak(y, 62400) < 0.45f);
    }
    {
        Vt2 v;
        std::vector<float> l = x, r = x;
        AllocationCounter c;
        for (size_t off = 0; off + 256 <= x.size(); off += 256) {
            float* ptr[2] = {l.data() + off, r.data() + off};
            AudioBlock b{ptr, 2, 256};
            v.vt.process(b, nullptr, nullptr, 0);
        }
        CHECK(c.count() == 0);
    }
    // old vs new on the off-key melody (hard): pitch error per note + discontinuities
    static const double notes[] = {60.0, 62.0, 64.0, 65.0, 67.0, 69.0, 71.0, 72.0};
    auto evaluate = [&](Processor& p, const char* name, int latency) {
        const auto y = runMono(p, x, {256});
        auto tr = vocal::detectPitch(y.data(), static_cast<int64_t>(y.size()), SR);
        double worst = 0;
        std::string perNote;
        for (int i = 0; i < 8; ++i) {
            const double t0 = 0.4 * i + 0.15 + latency / SR, t1 = 0.4 * i + 0.35 + latency / SR;
            std::vector<double> v;
            for (auto& f : tr.frames)
                if (f.voiced && f.time >= t0 && f.time < t1) v.push_back(f.midi);
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            worst = std::max(worst, std::fabs(v[v.size() / 2] - notes[i]) * 100);
            perNote += std::format(" {:+.0f}", (v[v.size() / 2] - notes[i]) * 100);
        }
        const auto rep = diag::detectClicks(y);
        std::printf("    %-14s worst note error %5.1f cents (per note:%s), clicks %zu (max score %.1f), latency %d samples\n", name, worst,
                    perNote.c_str(), rep.count(), rep.maxScore, latency);
        return worst;
    };
    RoyVocalTune oldVt;
    oldVt.setParam("keyRoot", 0.0f);
    oldVt.setParam("keyScale", 1.0f);
    oldVt.setParam("speed", 0.0f);
    oldVt.prepare(SR, 256);
    Vt2 newVt(SR, 0);
    newVt.vt.setParam("mode", static_cast<float>(RoyVocalTune2::Hard)); // like the old one: no vibrato logic, 0 ms
    newVt.vt.reset();
    const double eOld = evaluate(oldVt, "roy.vocaltune", oldVt.latencySamples());
    const double eNew = evaluate(newVt.vt, "roy.vocaltune2", newVt.vt.latencySamples());
    CHECK(eNew <= std::max(eOld, 5.0));
    for (int mode : {RoyVocalTune2::Natural, RoyVocalTune2::Modern}) {
        Vt2 v(SR, 20);
        v.vt.setParam("mode", static_cast<float>(mode));
        v.vt.reset();
        const double e = evaluate(v.vt, mode == RoyVocalTune2::Natural ? "vt2 Natural" : "vt2 Modern", v.vt.latencySamples());
        CHECK(e < 12.0); // natural modes keep more of the voice's own movement, but land on the notes
    }
}

TEST_CASE("vocaltune2", "VocalTune 2 in the engine: latency reported, delay compensation lines the channel up with the beat") {
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt{engine};
    Project p = makeNewProject("pdc", SR, 120.0);
    registerBuiltinProcessors();
    const std::string ac = addTrack(p, TrackType::Audio, "Vocal").channelId; // (addTrack may move earlier tracks)
    const std::string bc = addTrack(p, TrackType::Audio, "Beat").channelId;
    const std::string id = addMemoryAsset(p, rt, makeImpulse(SR, 1.0, 12000, 0.5f), "imp");
    for (auto& t : p.tracks)
        if (t.channelId == ac || t.channelId == bc) addClip(t, id, 0.0, 2.0);
    PluginSlot s;
    s.id = files::newId();
    s.typeId = "roy.vocaltune2";
    s.name = "VocalTune 2";
    p.findChannel(ac)->inserts.push_back(s);
    REQUIRE(rt.rebuild(p));
    // solo each track: the impulse must arrive at the same output sample
    auto renderSolo = [&](const std::string& muteChannel) {
        for (auto& t : p.tracks) p.findChannel(t.channelId)->mute = (t.channelId == muteChannel);
        REQUIRE(rt.rebuild(p));
        engine.transport().seek(0);
        engine.transport().play();
        auto out = render(engine, 48000);
        engine.transport().stop();
        return argmaxAbs(out[0]);
    };
    const size_t tVocal = renderSolo(bc);
    const size_t tBeat = renderSolo(ac);
    std::printf("    impulse at output: vocal (VocalTune 2) %zu, beat %zu\n", tVocal, tBeat);
    CHECK(tVocal == tBeat);
}
