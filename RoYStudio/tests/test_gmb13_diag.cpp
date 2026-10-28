// GMB 13.0 - audio diagnosis tools: click/discontinuity detector, block independence,
// device timing gaps (separate from the CPU-over-budget counter) and MEASUREMENTS of the
// existing RoyVocalTune (unchanged) on reference signals.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "audio/DeviceManager.h"
#include "commands/Commands.h"
#include "diag/CallbackTiming.h"
#include "diag/ClickDetector.h"
#include "effects/Special.h"

#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <thread>

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

std::vector<float> tone(double hz, double seconds, float amp, double sr = SR) {
    std::vector<float> v(static_cast<size_t>(sr * seconds));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / sr));
    return v;
}
std::vector<float> glide(double hz0, double hz1, double seconds, float amp) {
    std::vector<float> v(static_cast<size_t>(SR * seconds));
    double ph = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(v.size());
        ph += (hz0 * std::pow(hz1 / hz0, t)) / SR;
        v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * ph));
    }
    return v;
}
std::vector<float> vibrato(double hz, double rateHz, double cents, double seconds, float amp) {
    std::vector<float> v(static_cast<size_t>(SR * seconds));
    double ph = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        ph += hz * std::pow(2.0, cents / 1200.0 * std::sin(2 * std::numbers::pi * rateHz * t)) / SR;
        v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * ph));
    }
    return v;
}
// continuous sung melody (no silences), notes slightly off C major, 0.4 s each, 40 ms glides
std::vector<float> melody(double seconds = 3.2) {
    VoiceSpec v;
    v.sr = SR;
    v.seconds = seconds;
    v.midi = [](double t) {
        static const double notes[] = {60.3, 62.4, 64.35, 65.6, 67.3, 69.45, 71.3, 72.4};
        const int i = static_cast<int>(t / 0.4) % 8;
        const double local = std::fmod(t, 0.4);
        const double prev = notes[(i + 7) % 8];
        if (t >= 0.4 && local < 0.04) return prev + (notes[i] - prev) * (local / 0.04);
        return notes[i];
    };
    return makeVoice(v);
}

// processes a mono signal through a processor (dual mono, stereo block) with a block-size pattern
std::vector<float> runMono(Processor& p, const std::vector<float>& x, const std::vector<int>& blocks) {
    std::vector<float> l = x, r = x;
    size_t off = 0, bi = 0;
    while (off < x.size()) {
        const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(blocks[bi++ % blocks.size()]), x.size() - off));
        float* ptr[2] = {l.data() + off, r.data() + off};
        AudioBlock b{ptr, 2, k};
        p.process(b, nullptr, nullptr, 0);
        off += static_cast<size_t>(k);
    }
    return l;
}

const std::vector<std::vector<int>>& blockPatterns() {
    static const std::vector<std::vector<int>> b = {{64}, {256}, {512}, {562}, {1024}, {1, 7, 100, 333, 64, 1024, 3, 562}};
    return b;
}
std::string patternName(const std::vector<int>& b) { return b.size() == 1 ? std::to_string(b[0]) : std::string("irregular"); }
} // namespace

// ---------------------------------------------------------------------------------- click detector
TEST_CASE("gmb13", "click detector: smooth reference signals have no clicks") {
    struct Ref {
        const char* name;
        std::vector<float> x;
    };
    std::vector<Ref> refs = {{"sine 110 Hz", tone(110, 2, 0.5f)},     {"sine 440 Hz", tone(440, 2, 0.5f)},
                             {"sine 2 kHz", tone(2000, 2, 0.5f)},     {"glissando 100->1000 Hz", glide(100, 1000, 3, 0.5f)},
                             {"vibrato 220 Hz 6 Hz +-50 c", vibrato(220, 6, 50, 3, 0.5f)}, {"sung melody (mock voice)", melody()},
                             {"silence", std::vector<float>(96000, 0.0f)}, {"very quiet sine (-80 dBFS)", tone(440, 2, 1e-4f)}};
    for (auto& r : refs) {
        const auto rep = diag::detectClicks(r.x);
        std::printf("    %-30s clicks %zu, max score %.2f\n", r.name, rep.count(), rep.maxScore);
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
            for (auto& e : rep.events) near = near || std::llabs(e.sample - f.at) <= 3;
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
TEST_CASE("gmb13", "callback timing: regular callbacks give no gaps, a late callback is counted with its size") {
    diag::CallbackTiming t;
    t.reset();
    const int64_t period = static_cast<int64_t>(1e9 * 512 / 48000.0); // 10.67 ms
    int64_t now = 1'000'000'000;
    for (int i = 0; i < 1000; ++i) {
        t.onCallback(now, 512, 48000.0, true, true);
        now += period + (i % 3 == 0 ? 300'000 : -150'000); // +-0.3 ms scheduling jitter
    }
    auto s = t.snapshot();
    CHECK(s.callbacks == 1000);
    CHECK(s.timingGaps == 0);
    CHECK(s.inputMissing == 0);
    CHECK(s.minFrames == 512 && s.maxFrames == 512);
    // one callback 25 ms late, one without input, one with a different size
    now += 25'000'000;
    t.onCallback(now, 512, 48000.0, true, false);
    now += period;
    t.onCallback(now, 256, 48000.0, true, true);
    s = t.snapshot();
    CHECK(s.timingGaps == 1);
    CHECK_NEAR(s.maxGapMs, 25.0 + (period - 150'000) / 1e6 - 1e3 * 512 / 48000.0, 0.5);
    CHECK(s.inputMissing == 1);
    CHECK(s.sizeChanges == 1);
    CHECK(s.minFrames == 256);
    // realtime safe
    {
        AllocationCounter c;
        for (int i = 0; i < 1000; ++i) t.onCallback(now += period, 512, 48000.0, true, true);
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
        {"in-tune sine C4 (no correction)", tone(midiToHz(60), 2, 0.4f), 20},
        {"sharp sine C4 +40 c, speed 20", tone(midiToHz(60.4), 2, 0.4f), 20},
        {"sharp sine C4 +40 c, speed 0 (hard)", tone(midiToHz(60.4), 2, 0.4f), 0},
        {"melody off-key, speed 20", melody(), 20},
        {"melody off-key, speed 0 (hard)", melody(), 0},
        {"glissando A3->A4, speed 0", glide(220, 440, 3, 0.4f), 0},
        {"vibrato A3 +-50 c, speed 20", vibrato(220, 5.5, 50, 3, 0.4f), 20},
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
        std::printf("    %-38s input clicks %zu | output clicks %3zu, max score %6.1f\n", c.name, inRep.count(), rep.count(), rep.maxScore);
        CHECK(allFinite(y));
    }
}

TEST_CASE("gmb13", "MEASUREMENT: parameter changes during playback (existing RoyVocalTune)") {
    registerBuiltinProcessors();
    const auto x = melody();
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
