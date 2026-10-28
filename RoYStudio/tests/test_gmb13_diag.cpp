// GMB 13.0 - audio diagnosis tools: click/discontinuity detector, block independence,
// device timing gaps (separate from the CPU-over-budget counter) and MEASUREMENTS of the
// existing RoyVocalTune (unchanged) on reference signals.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"
#include "DiagSignals.h"

#include "audio/DeviceManager.h"
#include "commands/Commands.h"
#include "diag/CallbackTiming.h"
#include "diag/ClickDetector.h"
#include "diag/LatencyProbe.h"
#include "effects/Special.h"

#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <thread>

using namespace roy;
using namespace roytest;

using namespace roytest::gmb13;

// ---------------------------------------------------------------------------------- click detector
TEST_CASE("gmb13", "click detector: smooth reference signals have no clicks") {
    struct Ref {
        const char* name;
        std::vector<float> x;
    };
    std::vector<Ref> refs = {{"sine 110 Hz", tone(110, 2, 0.5f)},     {"sine 440 Hz", tone(440, 2, 0.5f)},
                             {"sine 2 kHz", tone(2000, 2, 0.5f)},     {"glissando 100->1000 Hz", glide(100, 1000, 3, 0.5f)},
                             {"vibrato 220 Hz 6 Hz +-50 c", vibrato(220, 6, 50, 3, 0.5f)}, {"sung melody (smooth voice)", melody()},
                             {"voice with vibrato", smoothVoice([](double t) { return 57.0 + 0.5 * std::sin(2 * std::numbers::pi * 5.5 * t); }, 3)},
                             {"silence", std::vector<float>(96000, 0.0f)}, {"very quiet sine (-80 dBFS)", tone(440, 2, 1e-4f)}};
    for (auto& r : refs) {
        const auto rep = diag::detectClicks(r.x);
        std::string where;
        for (size_t k = 0; k < std::min<size_t>(rep.events.size(), 8); ++k) where += std::format(" {}({:.0f})", rep.events[k].sample, rep.events[k].score);
        std::printf("    %-30s clicks %zu, max score %.2f%s\n", r.name, rep.count(), rep.maxScore, where.c_str());
        CHECK_MSG(rep.count() == 0, std::format("{}: {} false clicks (max score {:.2f})", r.name, rep.count(), rep.maxScore));
    }
}

TEST_CASE("gmb13", "click detector finds jumps, dropouts and block discontinuities at the right sample") {
    struct Fault {
        const char* name;
        int64_t at;
        std::function<void(std::vector<float>&)> apply;
    };
    const std::vector<Fault> faults = {
        {"single-sample spike +0.05", 30011, [](std::vector<float>& x) { x[30011] += 0.05f; }},
        {"dropout: 64 samples of silence", 40000, [](std::vector<float>& x) { std::fill(x.begin() + 40000, x.begin() + 40064, 0.0f); }},
        {"block jump: 0.5 ms skipped", 50000, [](std::vector<float>& x) { std::copy(x.begin() + 50024, x.end(), x.begin() + 50000); }},
        {"polarity flip", 61234, [](std::vector<float>& x) { for (size_t i = 61234; i < x.size(); ++i) x[i] = -x[i]; }},
        {"gain step -6 dB", 70000, [](std::vector<float>& x) { for (size_t i = 70000; i < x.size(); ++i) x[i] *= 0.5f; }},
    };
    for (const char* base : {"sine 440", "melody"}) {
        for (auto& f : faults) {
            auto x = std::string(base) == "melody" ? melody(2.0) : tone(440, 2, 0.5f);
            f.apply(x);
            const auto rep = diag::detectClicks(x);
            bool near = false;
            const int64_t len = std::string(f.name).rfind("dropout", 0) == 0 ? 64 : 0; // either edge of the silence
            for (auto& e : rep.events) near = near || (e.sample >= f.at - 3 && e.sample <= f.at + len + 3);
            std::printf("    %-8s %-34s events %zu, max score %.1f at %lld\n", base, f.name, rep.count(), rep.maxScore,
                        static_cast<long long>(rep.maxScoreSample));
            CHECK_MSG(near, std::format("{} / {}: not found at {}", base, f.name, f.at));
            CHECK_MSG(rep.count() <= 2, std::format("{} / {}: {} events (expected 1-2)", base, f.name, rep.count()));
        }
    }
}

TEST_CASE("gmb13", "click detector: hard clipping and extreme levels are measured, not crashing") {
    auto x = tone(220, 2, 1.0f);
    for (auto& v : x) v = std::clamp(v * 4.0f, -1.0f, 1.0f); // +12 dB into a hard clip
    const auto clip = diag::detectClicks(x);
    std::printf("    hard-clipped sine (+12 dB): events %zu, max score %.1f (corner of the clip, reported - clipping is a distortion)\n",
                clip.count(), clip.maxScore);
    CHECK(std::isfinite(clip.maxScore));
    std::vector<float> big = tone(220, 1, 1000.0f);
    const auto b = diag::detectClicks(big);
    CHECK(b.count() == 0);
    CHECK(diag::detectClicks(nullptr, 0).count() == 0);
}

// ---------------------------------------------------------------------------------- callback timing
TEST_CASE("gmb13", "callback timing: jitter, bursty delivery and clock drift give no gaps; a stall is counted with its size") {
    const double sr = 48000.0;
    const int64_t period = static_cast<int64_t>(1e9 * 512 / sr); // 10.67 ms
    auto runFor = [&](diag::CallbackTiming& t, int64_t& now, int n, auto&& nextDt) {
        for (int i = 0; i < n; ++i) {
            t.onCallback(now, 512, sr, true, true);
            now += nextDt(i);
        }
    };
    {
        diag::CallbackTiming t;
        int64_t now = 1'000'000'000;
        runFor(t, now, 3000, [&](int i) { return period + (i % 2 == 0 ? 2'000'000 : -2'000'000); }); // +-2 ms jitter
        CHECK(t.snapshot().timingGaps == 0);
    }
    {
        diag::CallbackTiming t; // WASAPI-like bursts: two callbacks back to back, then 2 periods pause
        int64_t now = 1'000'000'000;
        runFor(t, now, 3000, [&](int i) { return i % 2 == 0 ? int64_t{50'000} : 2 * period - 50'000; });
        const auto s = t.snapshot();
        std::printf("    bursty delivery: %llu gaps, longest interval %.1f ms\n", static_cast<unsigned long long>(s.timingGaps), s.maxIntervalMs);
        CHECK(s.timingGaps == 0);
    }
    {
        diag::CallbackTiming t; // sound card clock 200 ppm slower than the steady clock, 10 minutes
        int64_t now = 1'000'000'000;
        runFor(t, now, 56250, [&](int) { return static_cast<int64_t>(period * 1.0002); });
        CHECK(t.snapshot().timingGaps == 0);
    }
    diag::CallbackTiming t;
    int64_t now = 1'000'000'000;
    runFor(t, now, 200, [&](int i) { return period + (i % 3 == 0 ? 300'000 : -150'000); });
    auto s = t.snapshot();
    CHECK(s.callbacks == 200);
    CHECK(s.timingGaps == 0);
    CHECK(s.inputMissing == 0);
    CHECK(s.minFrames == 512 && s.maxFrames == 512);
    // the device stalls 25 ms (it runs out of audio), one callback without input, one with a different size
    now += 25'000'000;
    t.onCallback(now, 512, sr, true, false);
    now += period;
    t.onCallback(now, 256, sr, true, true);
    now += period / 2; // 256 frames = half a period later the next callback is due
    for (int i = 0; i < 50; ++i, now += period) t.onCallback(now, 512, sr, true, true);
    s = t.snapshot();
    std::printf("    25 ms stall: %llu gap(s), size %.2f ms\n", static_cast<unsigned long long>(s.timingGaps), s.maxGapMs);
    CHECK(s.timingGaps == 1);
    CHECK_NEAR(s.maxGapMs, 25.0, 1.0);
    CHECK(s.inputMissing == 1);
    CHECK(s.sizeChanges >= 1);
    CHECK(s.minFrames == 256);
    {
        AllocationCounter c;
        for (int i = 0; i < 1000; ++i) t.onCallback(now += period, 512, sr, true, true);
        CHECK(c.count() == 0);
    }
    t.reset();
    CHECK(t.snapshot().callbacks == 0);
}

TEST_CASE("gmb13", "device timing on the null backend: counters run, kept separate from CPU overloads") {
    AudioEngine engine;
    DeviceManager dm;
    AudioDeviceConfig cfg;
    cfg.backend = "null";
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    std::string err;
    if (!dm.open(cfg, engine, &err) || !dm.start(&err)) {
        std::printf("    null backend not available here: %s - SKIPPED\n", err.c_str());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    dm.stop();
    const auto s = dm.callbackTiming();
    std::printf("    null backend 0.7 s: %llu callbacks, %llu timing gaps, longest interval %.2f ms, frames %u..%u, CPU overloads %llu\n",
                static_cast<unsigned long long>(s.callbacks), static_cast<unsigned long long>(s.timingGaps), s.maxIntervalMs, s.minFrames,
                s.maxFrames, static_cast<unsigned long long>(engine.stats().overloads));
    CHECK(s.callbacks > 0);
    CHECK(s.callbacks == dm.callbackCount());
}

// ---------------------------------------------------------------------------------- block independence
TEST_CASE("gmb13", "block independence: existing RoyVocalTune gives the same output for every block size") {
    registerBuiltinProcessors();
    const auto x = melody();
    std::vector<float> ref;
    for (auto& bp : blockPatterns()) {
        RoyVocalTune vt;
        vt.setParam("keyRoot", 0.0f);
        vt.setParam("keyScale", 1.0f);
        vt.setParam("speed", 20.0f);
        vt.prepare(SR, 1024);
        const auto y = runMono(vt, x, bp);
        if (ref.empty()) {
            ref = y;
            continue;
        }
        const double d = diag::maxAbsDifference(ref, y);
        std::printf("    VocalTune block %-9s max diff to block 64: %.3g\n", patternName(bp).c_str(), d);
        CHECK(d <= 1e-6);
    }
}

TEST_CASE("gmb13", "block independence: the whole LIVE VOCAL chain in the engine (measured per block size)") {
    const auto mic = melody();
    std::vector<float> ref;
    double worst = 0;
    for (auto& bp : blockPatterns()) {
        AudioEngine engine;
        engine.prepare(SR, 1024);
        ProjectRuntime rt{engine};
        Project p = makeNewProject("blocks", SR, 120.0);
        UndoManager undo{p};
        CommandRegistry reg;
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        CommandContext ctx{p, undo, &rt};
        p.key = Key{0, ScaleType::Major};
        Track& t = addTrack(p, TrackType::Audio, "Vocal");
        const std::string id = t.id;
        REQUIRE(rt.rebuild(p));
        REQUIRE(reg.execute(ctx, "SetupLiveVocal", {{"trackId", id}}));
        REQUIRE(rt.rebuild(p));
        std::vector<float> out, l(1024), r(1024);
        size_t off = 0, bi = 0;
        while (off < mic.size()) {
            const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(bp[bi++ % bp.size()]), mic.size() - off));
            const float* in[1] = {mic.data() + off};
            float* o[2] = {l.data(), r.data()};
            engine.process(in, 1, o, 2, k);
            out.insert(out.end(), l.begin(), l.begin() + k);
            off += static_cast<size_t>(k);
        }
        CHECK(allFinite(out));
        if (ref.empty()) {
            ref = out;
            const auto c = diag::detectClicks(out);
            std::printf("    LIVE chain output (block 64): peak %.3f, clicks %zu, max score %.1f\n", peak(out), c.count(), c.maxScore);
            continue;
        }
        const double d = diag::maxAbsDifference(ref, out);
        worst = std::max(worst, d);
        std::printf("    LIVE chain block %-9s max diff to block 64: %.3g\n", patternName(bp).c_str(), d);
    }
    // measured, reported: block-size dependent effects (e.g. per-block smoothing) show up here
    std::printf("    worst difference over all block sizes: %.3g (%.1f dBFS)\n", worst, worst > 0 ? 20 * std::log10(worst) : -999.0);
    CHECK(std::isfinite(worst));
}

// ---------------------------------------------------------------------------------- measurements of the OLD VocalTune
TEST_CASE("gmb13", "MEASUREMENT: discontinuities produced by the existing RoyVocalTune (algorithm unchanged)") {
    registerBuiltinProcessors();
    struct Case {
        const char* name;
        std::vector<float> x;
        float speed;
    };
    const std::vector<Case> cases = {
        {"abrupt-start sine C4 (test-signal onset)", tone(midiToHz(60), 2, 0.4f), 20},
        {"in-tune sine C4 (no correction)", faded(tone(midiToHz(60), 2, 0.4f)), 20},
        {"sharp sine C4 +40 c, speed 20", faded(tone(midiToHz(60.4), 2, 0.4f)), 20},
        {"sharp sine C4 +40 c, speed 0 (hard)", faded(tone(midiToHz(60.4), 2, 0.4f)), 0},
        {"melody off-key, speed 20", faded(melody()), 20},
        {"melody off-key, speed 0 (hard)", faded(melody()), 0},
        {"phrase after 0.5 s silence, speed 0", withSilence(faded(melody(1.6)), 0.5), 0},
        {"glissando A3->A4, speed 0", faded(glide(220, 440, 3, 0.4f)), 0},
        {"vibrato A3 +-50 c, speed 20", faded(vibrato(220, 5.5, 50, 3, 0.4f)), 20},
        {"F#4 in C major (off-key), speed 0", faded(tone(midiToHz(66), 2, 0.4f)), 0},
        {"between E4/F4 (64.5), speed 0", faded(tone(midiToHz(64.5), 2, 0.4f)), 0},
    };

    for (auto& c : cases) {
        const auto inRep = diag::detectClicks(c.x);
        RoyVocalTune vt;
        vt.setParam("keyRoot", 0.0f);
        vt.setParam("keyScale", 1.0f);
        vt.setParam("speed", c.speed);
        vt.prepare(SR, 256);
        const auto y = runMono(vt, c.x, {256});
        const auto rep = diag::detectClicks(y);
        std::string where;
        for (size_t k = 0; k < std::min<size_t>(rep.events.size(), 4); ++k) where += std::format(" {:.3f}s", rep.events[k].sample / SR);
        std::printf("    %-38s input clicks %zu | output clicks %3zu, max score %6.1f |%s\n", c.name, inRep.count(), rep.count(), rep.maxScore,
                    where.c_str());
        CHECK(allFinite(y));
    }
}

TEST_CASE("gmb13", "MEASUREMENT: parameter changes during playback (existing RoyVocalTune)") {
    registerBuiltinProcessors();
    const auto x = faded(melody());
    struct Change {
        const char* name;
        const char* param;
        float from, to;
    };
    const std::vector<Change> changes = {{"speed 200 -> 0 ms", "speed", 200, 0}, {"strength 0 -> 1", "strength", 0, 1},
                                         {"key C -> F#", "keyRoot", 0, 6},      {"threshold 50 -> 0 c", "threshold", 50, 0}};
    for (auto& ch : changes) {
        RoyVocalTune vt;
        vt.setParam("keyRoot", 0.0f);
        vt.setParam("keyScale", 1.0f);
        vt.setParam("speed", 20.0f);
        vt.setParam(ch.param, ch.from);
        vt.prepare(SR, 256);
        std::vector<float> l = x, r = x;
        for (size_t off = 0; off < x.size(); off += 256) {
            if (off == 48000 + 256 * 37) vt.setParam(ch.param, ch.to); // mid-note, mid-block position
            const int k = static_cast<int>(std::min<size_t>(256, x.size() - off));
            float* ptr[2] = {l.data() + off, r.data() + off};
            AudioBlock b{ptr, 2, k};
            vt.process(b, nullptr, nullptr, 0);
        }
        const auto rep = diag::detectClicks(l);
        std::printf("    %-22s output clicks %3zu, max score %6.1f\n", ch.name, rep.count(), rep.maxScore);
        CHECK(allFinite(l));
    }
}

TEST_CASE("gmb13", "MEASUREMENT: existing RoyVocalTune at 44.1 kHz and 48 kHz, silence and overload recovery") {
    registerBuiltinProcessors();
    for (double sr : {44100.0, 48000.0}) {
        RoyVocalTune vt;
        vt.setParam("keyRoot", 0.0f);
        vt.setParam("keyScale", 1.0f);
        vt.setParam("speed", 20.0f);
        vt.prepare(sr, 512);
        auto x = tone(midiToHz(60.4), 2, 0.4f, sr);
        for (size_t i = 0; i < 480; ++i) x[i] *= static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(i) / 480));
        const auto y = runMono(vt, x, {512});
        const auto rep = diag::detectClicks(y);
        std::printf("    %.1f kHz sharp sine: detected %.2f MIDI (input 60.40), applied shift %.2f st, output clicks %zu (max %.1f)\n", sr / 1000,
                    vt.detectedMidi(), vt.appliedShift(), rep.count(), rep.maxScore);
        CHECK_NEAR(vt.detectedMidi(), 60.4, 0.1);
    }
    // silence: output silent and finite
    {
        RoyVocalTune vt;
        vt.prepare(SR, 256);
        std::vector<float> z(48000, 0.0f);
        const auto y = runMono(vt, z, {256});
        CHECK(peak(y) == 0.0f);
    }
    // +20 dB overload then normal: finite, back to normal level
    {
        RoyVocalTune vt;
        vt.setParam("keyRoot", 0.0f);
        vt.setParam("keyScale", 1.0f);
        vt.prepare(SR, 256);
        auto x = tone(midiToHz(60.4), 2, 0.4f);
        for (size_t i = 0; i < 48000; ++i) x[i] *= 10.0f;
        const auto y = runMono(vt, x, {256});
        CHECK(allFinite(y));
        std::printf("    overload +20 dB for 1 s, then normal: peak after 1.2 s %.3f (input 0.4)\n", peak(y, 57600));
        CHECK(peak(y, 57600) < 0.6f);
    }
    // the processing path of the old VocalTune does not allocate
    {
        RoyVocalTune vt;
        vt.prepare(SR, 256);
        auto x = melody(1.0);
        std::vector<float> l = x, r = x;
        AllocationCounter c;
        for (size_t off = 0; off + 256 <= x.size(); off += 256) {
            float* ptr[2] = {l.data() + off, r.data() + off};
            AudioBlock b{ptr, 2, 256};
            vt.process(b, nullptr, nullptr, 0);
        }
        CHECK(c.count() == 0);
    }
}

// ---------------------------------------------------------------------------------- 13.6 round-trip latency
TEST_CASE("gmb13", "round-trip latency: probe analysis finds the exact delay, rejects missing loopback") {
    for (double sr : {44100.0, 48000.0}) {
        const auto probe = diag::makeLatencyProbe(sr);
        for (int delay : {0, 137, 1234, 9000}) {
            std::vector<float> rec(static_cast<size_t>(diag::latencyProbeSeconds() * sr), 0.0f);
            Rng rng(11);
            for (size_t i = 0; i < probe.size() && i + static_cast<size_t>(delay) < rec.size(); ++i) rec[i + static_cast<size_t>(delay)] = -0.3f * probe[i]; // inverted, quieter
            for (auto& v : rec) v += static_cast<float>(rng.uniform(-0.01, 0.01));                                                                     // noise
            const auto r = diag::analyseLatencyProbe(probe, rec, sr);
            CHECK_MSG(r.ok && r.samples == delay, std::format("{} Hz delay {}: {} ({})", sr, delay, r.samples, r.message));
        }
        std::vector<float> silence(static_cast<size_t>(diag::latencyProbeSeconds() * sr), 0.0f);
        CHECK(!diag::analyseLatencyProbe(probe, silence, sr).ok);
        std::vector<float> noise = silence;
        Rng rng(4);
        for (auto& v : noise) v = static_cast<float>(rng.uniform(-0.3, 0.3));
        const auto rn = diag::analyseLatencyProbe(probe, noise, sr);
        std::printf("    %.0f Hz no loopback (noise only): ok=%d, %s\n", sr, rn.ok, rn.message.c_str());
        CHECK(!rn.ok);
    }
}

TEST_CASE("gmb13", "round-trip latency through the engine with a simulated loopback cable; no probe left in the output afterwards") {
    AudioEngine engine;
    engine.prepare(SR, 256);
    const int D = 777; // simulated converter + driver delay
    const auto probe = diag::makeLatencyProbe(SR);
    REQUIRE(engine.startLatencyProbe(probe, diag::latencyProbeSeconds()));
    std::vector<float> history(static_cast<size_t>(4 * SR), 0.0f); // everything the "sound card" played
    size_t t = 0;
    std::vector<float> l(256), r(256), in(256);
    while (engine.inputCaptureRunning() && t + 256 < history.size()) {
        for (int i = 0; i < 256; ++i) in[static_cast<size_t>(i)] = t + i >= static_cast<size_t>(D) ? history[t + i - D] : 0.0f;
        const float* ins[1] = {in.data()};
        float* outs[2] = {l.data(), r.data()};
        engine.process(ins, 1, outs, 2, 256);
        std::copy(l.begin(), l.end(), history.begin() + static_cast<long>(t));
        t += 256;
    }
    std::vector<float> cl, cr;
    REQUIRE(engine.takeInputCapture(cl, cr));
    const auto res = diag::analyseLatencyProbe(probe, cl, SR);
    std::printf("    engine loopback %d samples: %s\n", D, res.message.c_str());
    CHECK(res.ok);
    CHECK(res.samples == D);
    // after the probe the outputs are silent again (no probe leaking into the music)
    float* outs[2] = {l.data(), r.data()};
    engine.process(nullptr, 0, outs, 2, 256);
    CHECK(peak(l) == 0.0f);
}
