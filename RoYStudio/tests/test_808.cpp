// 808 LAB: phase control (start phase, phase reset, de-click), root detection (with cents),
// sampler auto-root, and the visual KICK <-> 808 analyzer on synthetic and project sounds.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "beat/Collision.h"
#include "commands/Commands.h"
#include "core/AudioBuffer.h"
#include "instruments/Bass808.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

std::vector<float> play808(Bass808& b, const std::vector<std::pair<int, int>>& notes /* (sample, note) */, int total) {
    const int block = 256;
    b.prepare(SR, block);
    AudioBuffer buf(2, block);
    std::vector<float> out;
    for (int pos = 0; pos < total; pos += block) {
        buf.clear();
        std::vector<NoteEvent> ev;
        for (auto [at, note] : notes)
            if (at >= pos && at < pos + block) {
                NoteEvent e;
                e.offset = at - pos;
                e.note = static_cast<int16_t>(note);
                ev.push_back(e);
            }
        auto blk = buf.block();
        b.process(blk, nullptr, ev.data(), static_cast<int>(ev.size()));
        out.insert(out.end(), buf.channel(0), buf.channel(0) + block);
    }
    return out;
}

std::vector<float> sine(double hz, double sec, double phase = 0.0, float amp = 0.8f, double decaySec = 0.3) {
    std::vector<float> v(static_cast<size_t>(sec * SR));
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = i / SR;
        v[i] = static_cast<float>(amp * std::exp(-t / decaySec) * std::sin(kTwoPi * hz * t + phase));
    }
    return v;
}
} // namespace

TEST_CASE("808", "phase reset: every hit starts identically; retrigger is de-clicked") {
    Bass808 b;
    b.setParam("legato", 0.0f);
    b.setParam("glide", 0.0f);
    b.setParam("decay", 200.0f);
    // two hits far apart (the first has fully decayed): identical waveforms with Phase Reset
    auto out = play808(b, {{0, 36}, {48000, 36}}, 96000);
    double diff = 0;
    for (size_t i = 0; i < 4800; ++i) diff = std::max(diff, static_cast<double>(std::fabs(out[i] - out[48000 + i])));
    CHECK_MSG(diff < 1e-4, std::format("hit-to-hit difference {}", diff));
    // start phase 90 degrees changes the first cycle
    Bass808 c;
    c.setParam("legato", 0.0f);
    c.setParam("startPhase", 90.0f);
    auto o90 = play808(c, {{0, 36}}, 4800);
    double d90 = 0;
    for (size_t i = 0; i < 480; ++i) d90 = std::max(d90, static_cast<double>(std::fabs(o90[i] - out[i])));
    CHECK(d90 > 0.01);
    // retrigger while sounding at the worst phase: no step larger than the signal's own slope allows
    Bass808 r;
    r.setParam("legato", 0.0f);
    r.setParam("decay", 3000.0f);
    r.setParam("startPhase", 90.0f);
    auto rt = play808(r, {{0, 36}, {12345, 36}}, 24000);
    double maxStep = 0, maxStepElsewhere = 0;
    for (size_t i = 12340; i < 12400; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(rt[i] - rt[i - 1])));
    for (size_t i = 6000; i < 12000; ++i) maxStepElsewhere = std::max(maxStepElsewhere, static_cast<double>(std::fabs(rt[i] - rt[i - 1])));
    CHECK_MSG(maxStep < 3.0 * maxStepElsewhere + 1e-3, std::format("retrigger step {} vs normal {}", maxStep, maxStepElsewhere));
    // without phase reset the old behaviour (free-running phase) is available
    Bass808 f;
    f.setParam("legato", 0.0f);
    f.setParam("phaseReset", 0.0f);
    f.setParam("decay", 200.0f);
    auto fr = play808(f, {{0, 36}, {48000 + 17, 36}}, 96000);
    double fd = 0;
    for (size_t i = 0; i < 4800; ++i) fd = std::max(fd, static_cast<double>(std::fabs(fr[i] - fr[48017 + i])));
    CHECK(fd > 1e-3);
}

TEST_CASE("808", "root detection finds the settled note and the cents offset") {
    for (int note : {31, 33, 36, 38, 41, 43}) {
        Bass808 b;
        b.setParam("pitchEnv", 12.0f); // strong initial pitch drop must not fool the detector
        b.setParam("decay", 2000.0f);
        auto x = play808(b, {{0, note}}, 48000);
        auto rd = beat::detectRoot(x.data(), static_cast<int64_t>(x.size()), SR);
        CHECK_MSG(rd.midiNote == note, std::format("note {} detected {} ({:.1f} Hz)", note, rd.midiNote, rd.hz));
        CHECK_MSG(std::fabs(rd.cents) < 10.0, std::format("note {} cents {:.1f}", note, rd.cents));
        CHECK(rd.confidence > 0.5);
    }
    Bass808 d;
    d.setParam("tune", 0.3f); // +30 cents
    d.setParam("decay", 2000.0f);
    auto x = play808(d, {{0, 36}}, 48000);
    auto rd = beat::detectRoot(x.data(), static_cast<int64_t>(x.size()), SR);
    CHECK(rd.midiNote == 36);
    CHECK_NEAR(rd.cents, 30.0, 6.0);
    CHECK(rd.noteName == "C2");
    // noise: no confident root
    std::vector<float> noise(48000);
    Rng rng(3);
    for (auto& v : noise) v = static_cast<float>(rng.uniform(-0.5, 0.5));
    CHECK(beat::detectRoot(noise.data(), static_cast<int64_t>(noise.size()), SR).confidence < 0.3);
}

TEST_CASE("808", "visual analyzer: frequency overlap, timing overlap and phase correlation") {
    const auto kick = sine(55.0, 0.6, 0.0, 0.8f, 0.08);
    // same pitch, same time, same phase: big overlap, positive correlation
    auto v = beat::analyzeKick808Visual(kick, sine(55.0, 1.0), SR, 0.0);
    CHECK(v.frequencyOverlap > 0.5);
    CHECK(v.timingOverlapMs > 50.0);
    CHECK(v.phaseCorrelation > 0.9);
    CHECK(v.timeMs.size() == v.kickEnvDb.size());
    CHECK(v.freqHz.size() == 96 && v.kickSpecDb.size() == 96);
    CHECK(v.freqHz.front() > 19.0f && v.freqHz.back() < 420.0f);
    // polarity flipped: strong negative correlation (cancellation)
    auto inv = sine(55.0, 1.0, kPi);
    CHECK(beat::analyzeKick808Visual(kick, inv, SR, 0.0).phaseCorrelation < -0.9);
    // 808 an octave+ higher and much later: little overlap either way
    auto far = beat::analyzeKick808Visual(kick, sine(140.0, 1.0, 0.0, 0.8f, 0.3), SR, 0.45);
    CHECK(far.frequencyOverlap < v.frequencyOverlap * 0.5);
    CHECK(far.timingOverlapMs < v.timingOverlapMs * 0.5);
    const json j = v.toJson();
    CHECK(j["kickEnvDb"].size() == v.kickEnvDb.size());
}

TEST_CASE("808", "AnalyzeKick808 uses the project's kick and 808 as arranged; sampler auto-root") {
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("808", SR, 120.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerBuiltinProcessors();
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt};
    auto run = [&](const std::string& id, const json& a) {
        const bool ok = reg.execute(ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s: %s\n", id.c_str(), ctx.error.c_str());
        return ok;
    };
    CHECK(!run("AnalyzeKick808", json::object())); // nothing to analyze yet: clear message
    REQUIRE(run("AddPattern", {{"steps", 16}}));
    const std::string pat = ctx.result["id"];
    REQUIRE(run("SetRowPattern", {{"patternId", pat}, {"voice", "kick"}, {"text", "x.......x......."}}));
    REQUIRE(run("AddTrack", {{"type", "beat"}, {"name", "Drums"}}));
    const std::string drums = ctx.result["id"];
    REQUIRE(run("AddPatternClip", {{"trackId", drums}, {"patternId", pat}, {"startBeat", 0.0}}));
    REQUIRE(run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"instrument", "roy.808"}}));
    const std::string bass = ctx.result["id"];
    REQUIRE(run("AddMidiClip", {{"trackId", bass}, {"startBeat", 0.0}, {"lengthBeats", 4.0}}));
    const std::string clip = ctx.result["id"];
    REQUIRE(run("AddNote", {{"clipId", clip}, {"pitch", 33}, {"startBeat", 2.5}, {"lengthBeats", 1.0}, {"wrongNoteMode", "off"}}));
    const size_t undoBefore = undo.undoCount();
    REQUIRE(run("AnalyzeKick808", json::object()));
    CHECK_NEAR(ctx.result["offsetMs"].get<double>(), 250.0, 1.0); // kick on beat 2, 808 half a beat later at 120 bpm
    CHECK(ctx.result["bassNote"] == 33);
    CHECK(ctx.result["visual"]["timeMs"].size() > 50);
    CHECK(ctx.result.contains("suggestions"));
    CHECK(undo.undoCount() == undoBefore); // analysis changes nothing and records no undo step
    REQUIRE(run("AnalyzeKick808", {{"offsetMs", 0.0}}));
    CHECK(ctx.result["visual"]["timingOverlapMs"].get<double>() > 20.0);

    // sampler auto-root: an 808 one-shot rendered at A1 (+30 cents)
    Bass808 b;
    b.setParam("tune", 0.3f);
    b.setParam("decay", 2000.0f);
    auto x = play808(b, {{0, 33}}, 48000);
    auto data = std::make_shared<AudioData>();
    data->sampleRate = SR;
    data->numChannels = 1;
    data->numFrames = static_cast<int64_t>(x.size());
    data->channels = {x};
    const std::string asset = addMemoryAsset(p, rt, data, "808_A1");
    REQUIRE(run("DetectRoot", {{"assetId", asset}}));
    CHECK(ctx.result["midiNote"] == 33);
    REQUIRE(run("LoadSampleIntoSampler", {{"trackId", bass}, {"assetId", asset}, {"autoRoot", true}}));
    const auto& zone = p.findTrack(bass)->instrument->state["zones"][0];
    CHECK(zone["root"] == 33);
    CHECK_NEAR(zone["tune"].get<double>(), -0.3, 0.06);
}
