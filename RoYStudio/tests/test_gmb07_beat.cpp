// GMB 07 - Beat Lab: step sequencer, drum machine, 808 Lab, collision analyzer.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "beat/Collision.h"
#include "beat/StepSequencer.h"
#include "dsp/Analysis.h"
#include "dsp/FFT.h"
#include "instruments/Bass808.h"
#include "instruments/Drums.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

std::vector<float> playInstrument(Processor& inst, std::vector<NoteEvent> events, int blocks, int block = 256) {
    inst.prepare(SR, block);
    AudioBuffer buf(2, block);
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) {
        buf.clear();
        std::vector<NoteEvent> now;
        for (auto e : events)
            if (e.offset >= b * block && e.offset < (b + 1) * block) {
                e.offset -= b * block;
                now.push_back(e);
            }
        auto blk = buf.block();
        inst.process(blk, nullptr, now.data(), static_cast<int>(now.size()));
        out.insert(out.end(), buf.channel(0), buf.channel(0) + block);
    }
    return out;
}

NoteEvent on(int note, int at, float vel = 1.0f, bool slide = false) {
    NoteEvent e;
    e.type = NoteEvent::NoteOn;
    e.note = static_cast<int16_t>(note);
    e.offset = at;
    e.velocity = vel;
    e.slide = slide;
    return e;
}
NoteEvent off(int note, int at) {
    NoteEvent e = on(note, at);
    e.type = NoteEvent::NoteOff;
    return e;
}

double bandShare(const std::vector<float>& x, size_t from, double lo, double hi) {
    // zero-padded copy: never read past the end of short renders
    std::vector<float> w(4096, 0.0f);
    for (size_t i = 0; i < w.size() && from + i < x.size(); ++i) w[i] = x[from + i];
    auto mag = dsp::magnitudeSpectrum(w.data(), 4096);
    double in = 0, all = 0;
    for (size_t i = 1; i < mag.size(); ++i) {
        const double f = static_cast<double>(i) * SR / 4096;
        all += mag[i] * mag[i];
        if (f >= lo && f < hi) in += mag[i] * mag[i];
    }
    return in / std::max(1e-20, all);
}
} // namespace

TEST_CASE("gmb07", "pattern length, duplicate, variation, swing") {
    Pattern p = makeDefaultPattern("A", 16);
    CHECK(p.rows.size() == 9);
    beat::toggleStep(p, 0, 0);
    beat::toggleStep(p, 1, 4);
    beat::setPatternLength(p, 32);
    CHECK(p.rows[0].steps.size() == 32);
    CHECK(p.rows[0].steps[16].on); // repeated content
    beat::setPatternLength(p, 64);
    CHECK(p.numSteps == 64);
    auto d = beat::duplicatePattern(p, "B");
    CHECK(d.id != p.id);
    CHECK(d.rows[0].id != p.rows[0].id);
    CHECK(d.rows[1].steps[4].on);
    auto v1 = beat::makeVariation(p, 3, 0.8f);
    auto v2 = beat::makeVariation(p, 3, 0.8f);
    int diffs = 0;
    for (size_t r = 0; r < p.rows.size(); ++r)
        for (size_t s = 0; s < p.rows[r].steps.size(); ++s) {
            diffs += v1.rows[r].steps[s].on != p.rows[r].steps[s].on;
            CHECK(v1.rows[r].steps[s].on == v2.rows[r].steps[s].on); // deterministic
        }
    CHECK(diffs > 0);
    CHECK(v1.rows[0].steps[0].on); // backbeat kept
    p.swing = 0.6f;
    CHECK_NEAR(beat::stepPosition(p, 1), 0.25 + 0.6 * 0.25 / 3.0, 1e-6);
    CHECK_NEAR(beat::stepPosition(p, 2), 0.5, 1e-12);
}

TEST_CASE("gmb07", "pattern expansion: probability, flam, roll, micro timing, mute/solo") {
    Pattern p = makeDefaultPattern("A", 16);
    p.rows[0].steps[0] = {true, 1.0f, 0, 0, 1.0f, 0.0f, false, 0};
    p.rows[0].steps[4] = {true, 1.0f, 0, 0, 1.0f, 0.2f, false, 0};   // late micro timing
    p.rows[1].steps[8] = {true, 0.9f, 0, 0, 1.0f, 0.0f, true, 0};    // flam
    p.rows[3].steps[12] = {true, 0.7f, 0.5f, 2.0f, 1.0f, 0, false, 4}; // roll x4, pan, pitch
    for (int i = 0; i < 16; ++i) p.rows[5].steps[static_cast<size_t>(i)] = {true, 0.5f, 0, 0, 0.5f, 0, false, 0}; // 50 % probability
    PatternClip clip{"c", p.id, 0.0, 64.0, false, false, 0}; // 16 repeats
    auto notes = expandPatternClip(p, clip, 42);
    auto countNote = [&](int n) { return std::count_if(notes.begin(), notes.end(), [&](auto& x) { return x.note == n; }); };
    CHECK(countNote(36) == 32);
    CHECK(countNote(38) == 32);   // flam doubles the hits
    CHECK(countNote(42) == 64);   // roll of 4
    const auto perc = countNote(47);
    CHECK(perc > 16 * 16 * 0.35);
    CHECK(perc < 16 * 16 * 0.65);
    auto again = expandPatternClip(p, clip, 42);
    CHECK(again.size() == notes.size()); // deterministic per clip
    bool microOk = false;
    for (auto& n : notes)
        if (n.note == 36 && std::fabs(n.beat - (1.0 + 0.2 * 0.25)) < 1e-9) microOk = true;
    CHECK(microOk);
    for (auto& n : notes)
        if (n.note == 42) {
            CHECK(n.pan == 0.5f);
            CHECK(n.pitch == 2.0f);
        }
    p.rows[3].solo = true;
    auto solo = expandPatternClip(p, clip, 42);
    for (auto& n : solo) CHECK(n.note == 42);
    p.rows[3].muted = true;
    CHECK(expandPatternClip(p, clip, 42).empty());
}

TEST_CASE("gmb07", "drum voices have the expected spectra") {
    RoyDrums d;
    auto kick = playInstrument(d, {on(36, 0)}, 40);
    CHECK(bandShare(kick, 1000, 20, 200) > 0.7);
    auto hat = playInstrument(d, {on(42, 0)}, 20);
    CHECK(bandShare(hat, 0, 5000, 24000) > 0.6);
    auto snare = playInstrument(d, {on(38, 0)}, 20);
    CHECK(peak(snare) > 0.1f);
    auto clap = playInstrument(d, {on(39, 0)}, 20);
    CHECK(peak(clap) > 0.1f);
    for (auto* v : {&kick, &hat, &snare, &clap}) CHECK(allFinite(*v));
    // closed hat chokes the open hat
    auto choke = playInstrument(d, {on(46, 0), on(42, 4800)}, 40);
    CHECK(rms(choke, 4800 + 4000, 4800 + 8000) < 0.003);
    auto open = playInstrument(d, {on(46, 0)}, 40);
    CHECK(rms(open, 4800 + 4000, 4800 + 8000) > 0.005);
    // velocity scales the output
    auto soft = playInstrument(d, {on(36, 0, 0.25f)}, 10);
    auto loud = playInstrument(d, {on(36, 0, 1.0f)}, 10);
    CHECK_NEAR(peak(soft) / peak(loud), 0.25, 0.02);
}

TEST_CASE("gmb07", "drum sample pads play assigned samples") {
    RoyDrums d;
    auto s = makeSine(SR, 1000.0, 0.1, 0.8f, 1);
    d.prepare(SR, 256);
    d.setSample(36, s);
    auto out = playInstrument(d, {on(36, 0)}, 10);
    CHECK(bandShare(out, 0, 900, 1100) > 0.9);
    d.setSample(36, nullptr);
    auto synth = playInstrument(d, {on(36, 0)}, 20);
    CHECK(bandShare(synth, 1000, 20, 200) > 0.7);
}

TEST_CASE("gmb07", "beat track plays a pattern through the engine") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("beat", SR, 120.0);
    Pattern pat = makeDefaultPattern("Main", 16);
    for (int s : {0, 4, 8, 12}) pat.rows[0].steps[static_cast<size_t>(s)].on = true; // four on the floor
    p.patterns.push_back(pat);
    const std::string bt = addTrack(p, TrackType::Beat, "Drums").id;
    p.findTrack(bt)->patternClips.push_back({files::newId(), pat.id, 0.0, 8.0, false, false, 0});
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, static_cast<int64_t>(SR * 4));
    auto onsets = dsp::detectOnsets(out[0].data(), static_cast<int64_t>(out[0].size()), SR);
    REQUIRE(onsets.size() == 8);
    for (size_t i = 0; i < onsets.size(); ++i) CHECK_NEAR(onsets[i].time, 0.5 * static_cast<double>(i), 0.006);
}

TEST_CASE("gmb07", "808: pitch, glide/slide, key lock, saturation") {
    Bass808 b;
    b.setParam("pitchEnv", 0.0f);
    b.setParam("saturation", 0.0f);
    b.setParam("softClip", 0.0f);
    b.setParam("decay", 3000.0f);
    auto a1 = playInstrument(b, {on(33, 0), off(33, 30000)}, 200); // A1 = 55 Hz
    CHECK_NEAR(beat::lowFundamental(a1.data(), static_cast<int64_t>(a1.size()), SR, 0.05), 55.0, 1.0);
    CHECK(allFinite(a1));
    // slide: second note glides instead of retriggering
    b.setParam("glide", 100.0f);
    b.prepare(SR, 256);
    AudioBuffer buf(2, 256);
    std::vector<NoteEvent> evs = {on(33, 0)};
    auto blk = buf.block();
    b.process(blk, nullptr, evs.data(), 1);
    for (int i = 0; i < 20; ++i) b.process(blk, nullptr, nullptr, 0);
    NoteEvent slide = on(40, 0, 1.0f, true);
    b.process(blk, nullptr, &slide, 1);
    const double mid = b.currentPitch();
    CHECK(mid > 33.0 && mid < 40.0); // still gliding after 256 samples
    for (int i = 0; i < 100; ++i) b.process(blk, nullptr, nullptr, 0);
    CHECK_NEAR(b.currentPitch(), 40.0, 0.05);
    // 808 KEY LOCK: C minor, C# is snapped
    b.setParam("keyLock", 1.0f);
    b.setParam("keyRoot", 0.0f);
    b.setParam("keyScale", 2.0f); // natural minor
    NoteEvent cs = on(37, 0);
    b.process(blk, nullptr, &cs, 1);
    CHECK(b.lastPlayedNote() == 36);
    b.setParam("keyLock", 0.0f);
    b.process(blk, nullptr, &cs, 1);
    CHECK(b.lastPlayedNote() == 37);
    // saturation / distortion add harmonics
    Bass808 clean, dirty;
    for (auto* x : {&clean, &dirty}) {
        x->setParam("pitchEnv", 0.0f);
        x->setParam("softClip", 0.0f);
        x->setParam("saturation", 0.0f);
        x->setParam("tone", 20000.0f);
    }
    dirty.setParam("distortion", 0.8f);
    auto c = playInstrument(clean, {on(36, 0)}, 60);
    auto d = playInstrument(dirty, {on(36, 0)}, 60);
    CHECK(bandShare(d, 4800, 150, 2000) > bandShare(c, 4800, 150, 2000) + 0.05);
    CHECK(peak(d) <= 1.01f);
}

TEST_CASE("gmb07", "instruments do not allocate on the audio thread") {
    RoyDrums d;
    Bass808 b;
    d.prepare(SR, 256);
    b.prepare(SR, 256);
    AudioBuffer buf(2, 256);
    std::vector<NoteEvent> evs;
    for (int n : {36, 38, 42, 46, 39, 49, 37, 47, 35}) evs.push_back(on(n, n));
    NoteEvent k = on(37, 3);
    long allocs;
    {
        AllocationCounter c;
        auto blk = buf.block();
        d.process(blk, nullptr, evs.data(), static_cast<int>(evs.size()));
        b.setParam("keyLock", 1.0f);
        b.process(blk, nullptr, &k, 1);
        for (int i = 0; i < 50; ++i) {
            d.process(blk, nullptr, nullptr, 0);
            b.process(blk, nullptr, nullptr, 0);
        }
        allocs = c.count();
    }
    CHECK(allocs == 0);
}

TEST_CASE("gmb07", "kick/808 collision analyzer") {
    const size_t n = static_cast<size_t>(0.6 * SR);
    std::vector<float> kick(n), bassIn(n), bassOut(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SR;
        kick[i] = static_cast<float>(0.8 * std::sin(kTwoPi * 50.0 * t) * std::exp(-t / 0.15));
        bassIn[i] = static_cast<float>(0.6 * std::sin(kTwoPi * 50.0 * t) * std::exp(-t / 0.6));
        bassOut[i] = -bassIn[i]; // opposite polarity -> cancellation
    }
    auto good = beat::analyzeKick808(kick, bassIn, SR);
    CHECK_NEAR(good.kickFundamentalHz, 50.0, 2.0);
    CHECK_NEAR(good.bassFundamentalHz, 50.0, 2.0);
    CHECK(good.overlapRatio > 0.5);
    CHECK(good.correlation > 0.9);
    CHECK(good.sumVsSeparateDb > 1.0);
    auto has = [](const beat::CollisionReport& r, const std::string& t) {
        for (auto& s : r.suggestions)
            if (s.type == t) return true;
        return false;
    };
    CHECK(has(good, "sidechain"));
    CHECK(has(good, "dynamic_eq"));
    CHECK(!has(good, "phase"));
    auto bad = beat::analyzeKick808(kick, bassOut, SR);
    CHECK(bad.correlation < -0.9);
    CHECK(bad.sumVsSeparateDb < -3.0);
    CHECK(bad.invertBassHelps);
    CHECK(has(bad, "phase"));
    CHECK(bad.combinedPeakDb < good.combinedPeakDb);
    // a late 808 that does not overlap produces no sidechain suggestion
    auto apart = beat::analyzeKick808(kick, bassIn, SR, 1.5);
    CHECK(apart.overlapRatio < 0.05);
    CHECK(!has(apart, "sidechain"));
    CHECK(apart.toJson()["suggestions"].is_array());
}
