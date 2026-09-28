// GMB 03 - Playlist / arrangement: clip editing verified against rendered audio,
// time-stretch, waveform cache.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "arrange/ClipOps.h"
#include "arrange/WaveformCache.h"
#include "dsp/FFT.h"
#include "dsp/TimeStretch.h"

#include <chrono>

using namespace roy;
using namespace roytest;

namespace {

// Dominant frequency via FFT peak with parabolic interpolation.
double dominantHz(const std::vector<float>& x, size_t from, int n, double sr) {
    dsp::FFT fft(n);
    auto mag = dsp::magnitudeSpectrum(x.data() + from, n);
    size_t best = 1;
    for (size_t i = 2; i + 1 < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    const double a = mag[best - 1], b = mag[best], c = mag[best + 1];
    const double d = 0.5 * (a - c) / (a - 2 * b + c);
    return (static_cast<double>(best) + d) * sr / n;
}

struct Fixture {
    AudioEngine engine;
    std::unique_ptr<ProjectRuntime> rt;
    Project p;
    std::string asset, trackId;
    Fixture(double seconds = 4.0) {
        engine.prepare(48000.0, 512);
        rt = std::make_unique<ProjectRuntime>(engine);
        p = makeNewProject("arr", 48000.0, 120.0);
        // a "ramp-modulated" sine: unique content at every sample
        auto d = makeSine(48000.0, 220.0, seconds, 0.5f);
        for (int64_t i = 0; i < d->numFrames; ++i) {
            const float env = 0.2f + 0.8f * static_cast<float>(i) / static_cast<float>(d->numFrames);
            d->channels[0][static_cast<size_t>(i)] *= env;
            d->channels[1][static_cast<size_t>(i)] *= env;
        }
        asset = addMemoryAsset(p, *rt, d);
        p.assets.back().frames = d->numFrames;
        trackId = addTrack(p, TrackType::Audio, "A").id;
    }
    Track& track() { return *p.findTrack(trackId); }
    std::vector<std::vector<float>> run(int64_t frames) {
        REQUIRE(rt->rebuild(p));
        return render(engine, frames);
    }
};

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, std::fabs(double(a[i]) - b[i]));
    return m;
}

} // namespace

TEST_CASE("gmb03", "split clip renders sample-identical") {
    Fixture f;
    auto& c = addClip(f.track(), f.asset, 0.0, 8.0);
    const std::string id = c.id;
    auto ref = f.run(48000 * 4);
    auto r = arrange::splitClip(f.p, id, 3.37);
    REQUIRE(r.ok);
    CHECK(f.track().audioClips.size() == 2);
    auto out = f.run(48000 * 4);
    CHECK(maxDiff(ref[0], out[0]) < 1e-6);
    // split a clip at its edge is refused
    CHECK(!arrange::splitClip(f.p, id, 0.0).ok);
}

TEST_CASE("gmb03", "move, trim, slip and duplicate") {
    Fixture f;
    const std::string id = addClip(f.track(), f.asset, 0.0, 4.0).id; // 2 s
    auto ref = f.run(48000 * 6);
    REQUIRE(arrange::moveClip(f.p, id, 2.0).ok); // +1 s
    auto moved = f.run(48000 * 6);
    std::vector<float> shifted(ref[0].size());
    for (size_t i = 48000; i < shifted.size(); ++i) shifted[i] = ref[0][i - 48000];
    CHECK(maxDiff(shifted, moved[0]) < 1e-6);

    // trim start by 0.5 beat: content stays at the same timeline position
    REQUIRE(arrange::trimClipStart(f.p, id, 2.5).ok);
    auto trimmed = f.run(48000 * 6);
    CHECK(peak(trimmed[0], 48000, 48000 + 11990) < 1e-6f);
    CHECK(maxDiff(std::vector<float>(moved[0].begin() + 60000, moved[0].end()),
                  std::vector<float>(trimmed[0].begin() + 60000, trimmed[0].end())) < 1e-6);
    // trim end cannot exceed the source
    REQUIRE(arrange::trimClipEnd(f.p, id, 100.0).ok);
    CHECK(f.p.findAudioClip(id)->endBeat() <= 2.0 + 8.0 + 1e-6);

    // slip: content moves inside the clip
    const double offBefore = f.p.findAudioClip(id)->sourceOffsetSec;
    REQUIRE(arrange::slipClip(f.p, id, -0.1).ok);
    CHECK_NEAR(f.p.findAudioClip(id)->sourceOffsetSec, offBefore + 0.1, 1e-9);
    CHECK(!arrange::slipClip(f.p, id, 100.0).ok);

    auto dup = arrange::duplicateClip(f.p, id);
    REQUIRE(dup.ok);
    CHECK_NEAR(f.p.findAudioClip(dup.newId)->startBeat, f.p.findAudioClip(id)->endBeat(), 1e-9);
}

TEST_CASE("gmb03", "locked clips and groups") {
    Fixture f;
    const std::string a = addClip(f.track(), f.asset, 0.0, 2.0).id;
    const std::string b = addClip(f.track(), f.asset, 4.0, 2.0).id;
    auto g = arrange::groupClips(f.p, {a, b});
    REQUIRE(g.ok);
    REQUIRE(arrange::moveClip(f.p, a, 1.0).ok);
    CHECK_NEAR(f.p.findAudioClip(b)->startBeat, 5.0, 1e-9);
    REQUIRE(arrange::setClipLocked(f.p, b, true).ok);
    CHECK(!arrange::moveClip(f.p, a, 2.0).ok); // group has a locked member
    CHECK(!arrange::splitClip(f.p, b, 5.5).ok);
    CHECK(!arrange::deleteClip(f.p, b).ok);
    REQUIRE(arrange::ungroup(f.p, g.newId).ok);
    REQUIRE(arrange::moveClip(f.p, a, 2.0).ok);
}

TEST_CASE("gmb03", "fades and crossfade are smooth") {
    Fixture f;
    const std::string a = addClip(f.track(), f.asset, 0.0, 2.0).id;
    const std::string b = addClip(f.track(), f.asset, 2.0, 2.0).id;
    f.p.findAudioClip(b)->sourceOffsetSec = 1.5023; // different phase -> a hard cut clicks
    auto hard = f.run(48000 * 2);
    auto r = arrange::crossfade(f.p, a, b, 0.25);
    REQUIRE(r.ok);
    CHECK(f.p.findAudioClip(a)->fadeOutBeats > 0.2);
    CHECK(f.p.findAudioClip(b)->fadeInBeats > 0.2);
    CHECK(f.p.findAudioClip(b)->startBeat < 2.0);
    auto xf = f.run(48000 * 2);
    // maximum sample-to-sample jump around the joint
    auto maxJump = [](const std::vector<float>& v, size_t s, size_t e) {
        double m = 0;
        for (size_t i = s + 1; i < e; ++i) m = std::max(m, std::fabs(double(v[i]) - v[i - 1]));
        return m;
    };
    CHECK(maxJump(xf[0], 47000, 49000) < maxJump(hard[0], 47000, 49000));
    CHECK(maxJump(xf[0], 40000, 56000) < 0.03);

    REQUIRE(arrange::setFades(f.p, a, 0.5, 0.5).ok);
    auto faded = f.run(48000);
    CHECK(std::fabs(faded[0][0]) < 1e-6);
}

TEST_CASE("gmb03", "clip gain, mute, reverse") {
    Fixture f;
    const std::string id = addClip(f.track(), f.asset, 0.0, 2.0).id;
    auto ref = f.run(48000);
    REQUIRE(arrange::setClipGain(f.p, id, -6.0206f).ok);
    auto half = f.run(48000);
    CHECK_NEAR(peak(half[0]), peak(ref[0]) * 0.5, 1e-3);
    f.p.findAudioClip(id)->reversed = true;
    auto rev = f.run(48000);
    CHECK_NEAR(rev[0][0], half[0][47999], 1e-6);
    REQUIRE(arrange::setClipMuted(f.p, id, true).ok);
    auto muted = f.run(48000);
    CHECK(peak(muted[0]) < 1e-9f);
}

TEST_CASE("gmb03", "time stretch keeps pitch, changes length") {
    auto sine = makeSine(48000.0, 440.0, 1.0, 0.5f, 1);
    auto st = dsp::timeStretch(sine->channels, 1.5, 48000.0);
    REQUIRE(st[0].size() == 72000);
    CHECK_NEAR(dominantHz(st[0], 20000, 8192, 48000.0), 440.0, 3.0);
    auto sh = dsp::pitchShift(sine->channels, 12.0, 48000.0);
    CHECK(sh[0].size() == 48000);
    CHECK_NEAR(dominantHz(sh[0], 10000, 8192, 48000.0), 880.0, 6.0);
    CHECK(allFinite(st[0]) && allFinite(sh[0]));
    CHECK(rms(st[0], 5000, 60000) > 0.3); // no big holes
}

TEST_CASE("gmb03", "stretched clip plays through the engine") {
    Fixture f(2.0);
    const std::string id = addClip(f.track(), f.asset, 0.0, 4.0).id; // 2 s
    REQUIRE(arrange::stretchClip(f.p, id, 1.5).ok);
    CHECK_NEAR(f.p.findAudioClip(id)->lengthBeats, 6.0, 1e-6);
    auto out = f.run(48000 * 4);
    CHECK(peak(out[0], 48000 * 2 + 1000, 48000 * 3 - 2000) > 0.1f); // audio continues past the original 2 s
    CHECK(peak(out[0], 48000 * 3 + 100) < 1e-6f);
    CHECK_NEAR(dominantHz(out[0], 30000, 8192, 48000.0), 220.0, 3.0);
}

TEST_CASE("gmb03", "waveform cache levels, persistence, speed") {
    auto d = makeSine(48000.0, 50.0, 600.0, 0.9f); // 10 minutes stereo
    WaveformCache c;
    const auto t0 = std::chrono::steady_clock::now();
    c.build(*d);
    const double buildSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_MSG(buildSec < 2.0, std::format("build took {} s", buildSec));
    std::vector<float> mn(1920), mx(1920);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) c.getPeaks(0, 0, static_cast<double>(d->numFrames), 1920, mn.data(), mx.data());
    const double querySec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / 100;
    CHECK_MSG(querySec < 0.005, std::format("full-view query {} s", querySec));
    CHECK_NEAR(mx[100], 0.9, 0.01);
    CHECK_NEAR(mn[100], -0.9, 0.01);
    // zoomed in: 200 samples over 100 px follows the waveform
    c.getPeaks(0, 1000, 1200, 100, mn.data(), mx.data());
    CHECK(mx[0] <= 0.9f + 1e-6f);
    auto dir = tempDir("peaks");
    WaveformStore store(dir);
    auto a = store.get("asset1", *d);
    CHECK(store.builtCount() == 1);
    WaveformStore store2(dir);
    auto b = store2.get("asset1", *d);
    CHECK(store2.loadedCount() == 1);
    CHECK(store2.builtCount() == 0);
    std::vector<float> m2(64), x2(64), m3(64), x3(64);
    a->getPeaks(1, 0, 48000 * 60, 64, m2.data(), x2.data());
    b->getPeaks(1, 0, 48000 * 60, 64, m3.data(), x3.data());
    CHECK(m2 == m3 && x2 == x3);
}

TEST_CASE("gmb03", "snap, time formats, markers, sections, loop") {
    Project p = makeNewProject("t", 48000.0, 120.0);
    CHECK_NEAR(arrange::snap(3.13, 0.25), 3.25, 1e-12);
    CHECK_NEAR(arrange::snap(3.12, 0.25), 3.0, 1e-12);
    CHECK(arrange::formatTime(p, 4.5, arrange::TimeFormat::BarsBeats) == "2.1.480");
    CHECK(arrange::formatTime(p, 120.0, arrange::TimeFormat::Seconds) == "1:00.000");
    CHECK(arrange::formatTime(p, 1.0, arrange::TimeFormat::Samples) == "24000");
    arrange::addMarker(p, 8.0, "B");
    arrange::addMarker(p, 4.0, "A");
    CHECK(p.markers[0].name == "A");
    arrange::addSection(p, "Hook", "hook", 32.0, 16.0);
    CHECK(p.sections[0].startBeat == 16.0);
    arrange::setLoop(p, true, 8.0, 4.0);
    CHECK(p.loop.startBeat == 4.0 && p.loop.endBeat == 8.0);
    auto& t = addTrack(p, TrackType::Audio, "x");
    const std::string tid = t.id;
    addClip(*p.findTrack(tid), "a", 0.0, 2.0);
    addClip(*p.findTrack(tid), "a", 10.0, 2.0);
    CHECK(arrange::clipsInRange(p, {tid}, 1.0, 5.0).size() == 1);
}
