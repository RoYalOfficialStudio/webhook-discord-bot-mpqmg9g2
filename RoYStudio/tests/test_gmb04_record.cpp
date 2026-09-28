// GMB 04 - Recording engine: takes, loop recording, punch, latency
// compensation, comping, clipping warning, never-lose recovery.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "io/AudioFile.h"
#include "record/Recorder.h"
#include "record/Takes.h"

#include <set>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {

constexpr double SR = 48000.0;

// Feeds `input` (mono, generated per timeline-independent frame counter) through
// the engine for `frames` frames with the given block size.
struct Rig {
    AudioEngine engine;
    Recorder rec;
    std::unique_ptr<ProjectRuntime> rt;
    Project p;
    std::string trackId;
    fs::path dir;
    int64_t fed = 0;
    std::function<float(int64_t)> signal = [](int64_t i) { return 0.5f * static_cast<float>(std::sin(kTwoPi * 330.0 * i / SR)); };

    explicit Rig(const std::string& name) {
        dir = tempDir(name);
        engine.prepare(SR, 256);
        rt = std::make_unique<ProjectRuntime>(engine);
        p = makeNewProject("rec", SR, 120.0);
        trackId = addTrack(p, TrackType::Audio, "Lead Vox").id;
        p.findTrack(trackId)->armed = true;
        p.findTrack(trackId)->inputLeft = 0;
        rec.prepare(SR, 256);
        rec.setOutputFolder(dir / "Audio");
        rec.setTracks(takes::recordConfig(p));
        engine.setInputListener(&rec);
        REQUIRE(rt->rebuild(p));
    }
    ~Rig() { engine.setInputListener(nullptr); }
    void run(int64_t frames, int block = 256) {
        std::vector<float> in(static_cast<size_t>(block)), l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
        const float* ins[1] = {in.data()};
        float* outs[2] = {l.data(), r.data()};
        for (int64_t done = 0; done < frames; done += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, frames - done));
            for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = signal(fed + i);
            engine.process(ins, 1, outs, 2, n);
            fed += n;
        }
    }
};

} // namespace

TEST_CASE("gmb04", "record a take into its own file") {
    Rig g("rec_basic");
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(48000);
    g.rec.stopRecording();
    g.engine.transport().stop();
    g.run(256);
    auto takes = g.rec.collectFinishedTakes();
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].frames == 48000);
    CHECK(takes[0].timelineStart == 0);
    CHECK(fs::exists(takes[0].path));
    AudioData d;
    REQUIRE(readAudioFile(takes[0].path, d));
    CHECK(d.numFrames == 48000);
    double maxErr = 0;
    for (int64_t i = 0; i < d.numFrames; ++i) maxErr = std::max(maxErr, std::fabs(double(d.channels[0][static_cast<size_t>(i)]) - g.signal(i)));
    CHECK(maxErr < 1e-5); // 24-bit
    CHECK(g.rec.droppedSamples() == 0);
}

TEST_CASE("gmb04", "recorded take plays back at the right position") {
    Rig g("rec_playback");
    g.rec.setLatencyCompensation(128);
    g.engine.transport().seek(24000);
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(24000);
    g.rec.stopRecording();
    g.engine.transport().stop();
    g.run(256);
    auto takes = g.rec.collectFinishedTakes();
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].timelineStart == 24000 - 128);
    const std::string takeId = takes::addRecordedTake(g.p, takes[0], g.dir);
    REQUIRE(!takeId.empty());
    const Track& t = *g.p.findTrack(g.trackId);
    REQUIRE(t.takes.size() == 1);
    REQUIRE(t.comp.size() == 1);
    CHECK(g.p.assets.back().kind == "recording");
    CHECK(!g.p.assets.back().sha256.empty());
    g.rt->setProjectDirectory(g.dir);
    g.p.findTrack(g.trackId)->armed = false;
    REQUIRE(g.rt->rebuild(g.p));
    auto out = render(g.engine, 60000);
    // latency-compensated: the take starts 128 samples earlier than it was captured
    CHECK(peak(out[0], 0, 24000 - 128 - 64) < 1e-6f);
    CHECK(peak(out[0], 24000, 47000) > 0.3f);
    CHECK_NEAR(out[0][30000], g.signal(30000 - (24000 - 128)), 2e-3); // take frame k plays at timeline start + k
}

TEST_CASE("gmb04", "loop recording creates one take per pass") {
    Rig g("rec_loop");
    g.engine.transport().setLoop(true, 0, 24000);
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(24000 * 3 + 5000);
    g.rec.stopRecording();
    g.engine.transport().stop();
    g.run(256);
    auto takes = g.rec.collectFinishedTakes();
    REQUIRE(takes.size() == 4);
    std::set<std::string> paths;
    for (size_t i = 0; i < takes.size(); ++i) {
        CHECK(takes[i].loopPass == static_cast<int>(i));
        CHECK(takes[i].timelineStart == 0);
        paths.insert(takes[i].path.string());
    }
    CHECK(paths.size() == 4); // every take has its own file
    CHECK(takes[0].frames == 24000);
    CHECK(takes[3].frames == 5000);
    for (auto& t : takes) takes::addRecordedTake(g.p, t, g.dir);
    CHECK(g.p.findTrack(g.trackId)->takes.back().lane == 3);
}

TEST_CASE("gmb04", "punch in and out") {
    Rig g("rec_punch");
    g.rec.setPunch(true, 12000, 36000);
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(48000);
    g.rec.stopRecording();
    auto takes = g.rec.collectFinishedTakes();
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].timelineStart == 12000);
    CHECK(takes[0].frames == 24000);
    AudioData d;
    REQUIRE(readAudioFile(takes[0].path, d));
    CHECK_NEAR(d.channels[0][100], g.signal(12100), 1e-5);
}

TEST_CASE("gmb04", "existing files are never overwritten") {
    Rig g("rec_nooverwrite");
    fs::create_directories(g.dir / "Audio");
    for (int round = 0; round < 2; ++round) {
        REQUIRE(g.rec.startRecording());
        g.engine.transport().play();
        g.run(4800);
        g.rec.stopRecording();
        g.engine.transport().stop();
        g.run(256);
    }
    auto takes = g.rec.collectFinishedTakes();
    REQUIRE(takes.size() == 2);
    CHECK(takes[0].path != takes[1].path);
    // writer refuses to overwrite
    WavWriter w;
    std::string err;
    CHECK(!w.open(takes[0].path, SR, 1, SampleFormat::Pcm24, false, &err));
    CHECK(fs::file_size(takes[0].path) > 44);
}

TEST_CASE("gmb04", "comping selects ranges across takes") {
    Track t;
    t.takes = {{"A", "a1", "Take 1", 0, 0.0, 8.0, "", 0}, {"B", "a2", "Take 2", 1, 0.0, 8.0, "", 1}};
    REQUIRE(takes::compWholeTake(t, "A"));
    REQUIRE(takes::compSelect(t, "B", 2.0, 4.0));
    REQUIRE(t.comp.size() == 3);
    CHECK(t.comp[0].takeId == "A");
    CHECK(t.comp[1].takeId == "B");
    CHECK(t.comp[1].startBeat == 2.0);
    CHECK(t.comp[2].takeId == "A");
    CHECK(t.comp[2].startBeat == 4.0);
    REQUIRE(takes::compSelect(t, "A", 1.0, 5.0));
    CHECK(t.comp.size() == 1); // merged back
    CHECK(!takes::compSelect(t, "B", 9.0, 12.0)); // outside the take
    CHECK(takes::removeTake(t, "A"));
    CHECK(t.comp.empty());
}

TEST_CASE("gmb04", "flatten comp keeps audio identical") {
    Rig g("rec_flatten");
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(48000);
    g.rec.stopRecording();
    g.engine.transport().stop();
    g.run(256);
    for (auto& t : g.rec.collectFinishedTakes()) takes::addRecordedTake(g.p, t, g.dir);
    g.p.findTrack(g.trackId)->armed = false;
    g.rt->setProjectDirectory(g.dir);
    REQUIRE(g.rt->rebuild(g.p));
    auto a = render(g.engine, 48000);
    auto ids = takes::flattenComp(g.p, g.trackId);
    CHECK(ids.size() == 1);
    CHECK(g.p.findTrack(g.trackId)->comp.empty());
    REQUIRE(g.rt->rebuild(g.p));
    auto b = render(g.engine, 48000);
    double m = 0;
    for (size_t i = 0; i < a[0].size(); ++i) m = std::max(m, std::fabs(double(a[0][i]) - b[0][i]));
    CHECK(m < 1e-6);
}

TEST_CASE("gmb04", "input clipping warning and meter") {
    Rig g("rec_clip");
    g.signal = [](int64_t i) { return (i % 100) == 0 ? 1.0f : 0.1f; };
    g.run(4800);
    CHECK(g.rec.clipCount(g.trackId) == 48);
    CHECK_NEAR(g.rec.inputPeak(g.trackId), 1.0, 1e-6);
    CHECK(g.rec.inputPeak(g.trackId) == 0.0f); // reset on read
}

TEST_CASE("gmb04", "never-lose: recover a performance without pressing record") {
    Rig g("rec_neverlose");
    g.rec.setNeverLoseSeconds(30.0);
    g.rec.setTracks(takes::recordConfig(g.p));
    // 2 s silence, 3 s "performance" (with a short breath gap), 4 s silence - transport rolling, NOT recording
    g.signal = [](int64_t i) {
        const double t = static_cast<double>(i) / SR;
        if (t < 2.0 || t >= 5.0) return 0.0f;
        if (t > 3.0 && t < 3.5) return 0.0f; // breath gap
        return 0.4f * static_cast<float>(std::sin(kTwoPi * 220.0 * t));
    };
    g.engine.transport().seek(96000); // timeline starts at 2 s
    g.engine.transport().play();
    g.run(static_cast<int64_t>(9 * SR));
    CHECK(g.rec.collectFinishedTakes().empty());
    CHECK_NEAR(g.rec.neverLoseSecondsAvailable(g.trackId), 9.0, 0.01);
    std::string err;
    auto r = g.rec.recoverLastPerformance(g.trackId, true, &err);
    REQUIRE(r.has_value());
    CHECK(fs::exists(r->path));
    CHECK_NEAR(static_cast<double>(r->frames) / SR, 3.5, 0.05); // 3 s + 2 x 0.25 s padding
    REQUIRE(r->hasTimeline);
    // performance began 2 s after the timeline start (96000) minus 0.25 s padding
    CHECK_NEAR(static_cast<double>(r->timelineStart), 96000 + 2.0 * SR - 0.25 * SR, 480.0);
    const std::string clip = takes::addRecoveredPerformance(g.p, *r, g.dir, 0.0);
    REQUIRE(!clip.empty());
    CHECK(g.p.assets.back().kind == "recovery");
    CHECK_NEAR(g.p.findAudioClip(clip)->startBeat, g.p.tempo.secondsToBeat(3.75), 0.02);
}

TEST_CASE("gmb04", "never-lose memory is bounded") {
    Recorder rec;
    rec.prepare(96000.0, 256);
    rec.setNeverLoseSeconds(3600.0);
    std::vector<RecordTrackConfig> many;
    for (int i = 0; i < 16; ++i) many.push_back({"t" + std::to_string(i), "T", i * 2, i * 2 + 1, 0.0f});
    rec.setTracks(many);
    // 16 stereo tracks x 1 h at 96 kHz would be 44 GB; the cap keeps it at <= 512 MB total.
    const double perTrack = rec.neverLoseSecondsAvailable("t0"); // nothing written yet
    CHECK(perTrack == 0.0);
    CHECK(Recorder::kMaxNeverLoseBytes <= 512ull * 1024 * 1024);
}

TEST_CASE("gmb04", "recording path does not allocate on the audio thread") {
    Rig g("rec_rt");
    REQUIRE(g.rec.startRecording());
    g.engine.transport().play();
    g.run(2048); // warm-up
    std::vector<float> in(256, 0.1f), l(256), r(256);
    const float* ins[1] = {in.data()};
    float* outs[2] = {l.data(), r.data()};
    long allocs = 0;
    {
        AllocationCounter c;
        for (int i = 0; i < 200; ++i) g.engine.process(ins, 1, outs, 2, 256);
        allocs = c.count();
    }
    CHECK_MSG(allocs == 0, std::format("{} allocations", allocs));
    g.rec.stopRecording();
}
