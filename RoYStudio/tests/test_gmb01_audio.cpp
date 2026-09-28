// GMB 01 - Audio Foundation tests.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "audio/DeviceManager.h"
#include "audio/Metronome.h"
#include "audio/TempoMap.h"
#include "audio/Transport.h"

#include <chrono>
#include <thread>

using namespace roy;
using namespace roytest;

namespace {

// TEST processor: gain with an artificial latency (delay), to test PDC.
class LatentGain : public Processor {
public:
    explicit LatentGain(int latency) : Processor({{"gain", "Gain", 0.0f, 2.0f, 1.0f}}), latency_(latency) {}
    std::string typeId() const override { return "test.latent"; }
    void prepare(double sr, int maxBlock) override {
        Processor::prepare(sr, maxBlock);
        line_.setMaxDelay(latency_);
        line_.setDelay(latency_);
    }
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override {
        line_.process(io.channel(0), io.channel(1), io.numFrames);
        io.applyGain(p(0));
    }
    int latencySamples() const override { return latency_; }
private:
    int latency_;
    DelayLine line_;
};

// TEST processor: deliberately produces NaN to check the output guard.
class NanMaker : public Processor {
public:
    std::string typeId() const override { return "test.nan"; }
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override {
        io.channel(0)[0] = std::numeric_limits<float>::quiet_NaN();
    }
};

void registerTestProcessors() {
    static bool done = false;
    if (done) return;
    done = true;
    ProcessorFactory::instance().add("test.latent100", "Latent 100", false, [] { return std::make_unique<LatentGain>(100); });
    ProcessorFactory::instance().add("test.nan", "NaN", false, [] { return std::make_unique<NanMaker>(); });
}

} // namespace

TEST_CASE("gmb01", "tempo map conversions") {
    TempoMap t(120.0);
    CHECK_NEAR(t.beatToSeconds(4.0), 2.0, 1e-12);
    CHECK_NEAR(t.secondsToBeat(2.0), 4.0, 1e-12);
    CHECK_NEAR(t.beatToSample(1.0, 48000.0), 24000.0, 1e-9);
    t.addTempoEvent(8.0, 60.0); // 8 beats at 120 = 4s, then 1 beat per second
    CHECK_NEAR(t.beatToSeconds(10.0), 6.0, 1e-12);
    CHECK_NEAR(t.secondsToBeat(6.0), 10.0, 1e-12);
    CHECK_NEAR(t.tempoAt(9.0), 60.0, 1e-12);
    for (double b : {0.0, 0.5, 3.3, 7.99, 8.0, 12.25, 100.0}) CHECK_NEAR(t.secondsToBeat(t.beatToSeconds(b)), b, 1e-9);

    TempoMap s(90.0, 6, 8);
    CHECK_NEAR(s.barToBeat(1), 3.0, 1e-12);
    auto bb = s.beatToBarBeat(4.5);
    CHECK(bb.bar == 1);
    CHECK_NEAR(bb.beatInBar, 3.0, 1e-9); // 1.5 quarter notes = 3 eighths
    s.addTimeSignature(2, 4, 4);
    CHECK_NEAR(s.barToBeat(3), 3.0 + 3.0 + 4.0, 1e-12);
    CHECK(s.beatToBarBeat(10.0).bar == 3);
}

TEST_CASE("gmb01", "transport play pause stop seek") {
    Transport t;
    Transport::Segment seg[8];
    int n = t.advance(256, seg, 8);
    CHECK(n == 1);
    CHECK(!seg[0].rolling);
    t.seek(1000);
    t.play();
    n = t.advance(256, seg, 8);
    CHECK(n == 1);
    CHECK(seg[0].rolling);
    CHECK(seg[0].timelineStart == 1000);
    CHECK(t.position() == 1256);
    t.pause();
    t.advance(256, seg, 8);
    CHECK(t.position() == 1256);
    CHECK(t.state() == Transport::State::Paused);
    t.play();
    t.advance(100, seg, 8);
    CHECK(t.position() == 1356);
    t.stop();
    t.advance(100, seg, 8);
    CHECK(t.position() == 1000); // return to start
    CHECK(t.state() == Transport::State::Stopped);
}

TEST_CASE("gmb01", "transport loop splits blocks") {
    Transport t;
    Transport::Segment seg[8];
    t.setLoop(true, 100, 300);
    t.seek(250);
    t.play();
    int n = t.advance(128, seg, 8);
    REQUIRE(n == 2);
    CHECK(seg[0].timelineStart == 250);
    CHECK(seg[0].numFrames == 50);
    CHECK(seg[1].timelineStart == 100);
    CHECK(seg[1].numFrames == 78);
    CHECK(seg[1].blockOffset == 50);
    CHECK(seg[1].jumped);
    CHECK(t.position() == 178);
    CHECK(t.loopCount() == 1);
    // A loop shorter than the block wraps multiple times.
    t.setLoop(true, 0, 40);
    t.seek(0);
    n = t.advance(128, seg, 8);
    int total = 0;
    for (int i = 0; i < n; ++i) total += seg[i].numFrames;
    CHECK(total == 128);
    CHECK(n == 4);
}

TEST_CASE("gmb01", "count-in and pre-roll") {
    Transport t;
    Transport::Segment seg[8];
    t.setCountInSamples(300);
    t.seek(1000);
    t.play();
    int n = t.advance(256, seg, 8);
    CHECK(n == 1);
    CHECK(seg[0].countIn);
    CHECK(!seg[0].rolling);
    CHECK(seg[0].timelineStart == 700);
    CHECK(t.isCountingIn());
    n = t.advance(256, seg, 8);
    REQUIRE(n == 2);
    CHECK(seg[0].countIn);
    CHECK(seg[0].numFrames == 44);
    CHECK(seg[1].rolling);
    CHECK(seg[1].timelineStart == 1000);
    CHECK(!t.isCountingIn());

    Transport p;
    p.setPreRollSamples(500);
    p.seek(2000);
    p.play();
    p.advance(64, seg, 8);
    CHECK(seg[0].rolling);
    CHECK(seg[0].timelineStart == 1500);
}

TEST_CASE("gmb01", "metronome clicks on every beat") {
    Metronome m;
    m.prepare(48000.0);
    m.setEnabled(true);
    TempoMap tempo(120.0);
    std::vector<float> l(48000 * 4), r(48000 * 4);
    for (int64_t pos = 0; pos < 48000 * 4; pos += 256) {
        const int n = static_cast<int>(std::min<int64_t>(256, 48000 * 4 - pos));
        m.render(tempo, pos, n, l.data() + pos, r.data() + pos, false);
    }
    CHECK(m.clicksTriggered() == 8); // 4 s at 120 bpm
    // Accent (downbeat) is louder/higher than normal clicks; both present.
    CHECK(peak(l, 0, 2000) > 0.1f);
    CHECK(peak(l, 24000, 26000) > 0.1f);
    CHECK(peak(l, 12000, 23990) < 1e-6f);
}

TEST_CASE("gmb01", "engine plays a clip bit-exact at unity") {
    AudioEngine engine;
    engine.prepare(48000.0, 512);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t", 48000.0, 120.0);
    auto sine = makeSine(48000.0, 440.0, 1.0);
    auto id = addMemoryAsset(p, rt, sine);
    auto& tr = addTrack(p, TrackType::Audio, "Vocal");
    addClip(tr, id, 0.0, 2.0); // 2 beats = 1 s
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 48000);
    REQUIRE(out.size() == 2);
    double maxErr = 0;
    for (size_t i = 0; i < out[0].size(); ++i) maxErr = std::max(maxErr, std::fabs(double(out[0][i]) - sine->channels[0][i]));
    CHECK(maxErr < 1e-5);
    CHECK(allFinite(out[0]));
}

TEST_CASE("gmb01", "sample rates and buffer sizes give identical output") {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        std::vector<float> reference;
        for (int block : {32, 64, 128, 256, 512, 1024}) {
            AudioEngine engine;
            engine.prepare(sr, 1024);
            ProjectRuntime rt(engine);
            Project p = makeNewProject("t", sr, 120.0);
            auto id = addMemoryAsset(p, rt, makeSine(sr, 1000.0, 0.5));
            auto& tr = addTrack(p, TrackType::Audio, "A");
            addClip(tr, id, 0.25, 0.5);
            p.findChannel(tr.channelId)->gainDb = -3.0f;
            p.findChannel(tr.channelId)->pan = 0.3f;
            REQUIRE(rt.rebuild(p));
            auto out = render(engine, static_cast<int64_t>(sr * 0.6), block);
            if (reference.empty()) {
                reference = out[1];
                CHECK(peak(reference) > 0.1f);
            } else {
                double maxErr = 0;
                for (size_t i = 0; i < reference.size(); ++i) maxErr = std::max(maxErr, std::fabs(double(out[1][i]) - reference[i]));
                CHECK_MSG(maxErr < 1e-6, std::format("sr {} block {} err {}", sr, block, maxErr));
            }
        }
    }
}

TEST_CASE("gmb01", "routing busses sends mute solo") {
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeSine(48000.0, 200.0, 1.0, 0.5f));
    const std::string vocalsBus = p.channels[1].id; // VOCALS
    const std::string fxBus = p.channels[4].id;     // FX
    auto& t1 = addTrack(p, TrackType::Audio, "Lead", vocalsBus);
    addClip(t1, id, 0, 2);
    p.findChannel(vocalsBus)->gainDb = -6.0206f; // half amplitude
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 24000);
    CHECK_NEAR(peak(out[0]), 0.25, 0.005);

    // post-fader send to FX at 0 dB doubles level at master (bus -6 dB + send 0 dB into FX)
    Send s;
    s.id = files::newId();
    s.targetChannelId = fxBus;
    s.levelDb = 0.0f;
    p.findChannel(t1.channelId)->sends.push_back(s);
    REQUIRE(rt.rebuild(p));
    out = render(engine, 24000);
    CHECK_NEAR(peak(out[0]), 0.75, 0.01);

    // mute the track
    p.findChannel(t1.channelId)->mute = true;
    rt.syncParams(p);
    out = render(engine, 24000);
    CHECK(peak(out[0], 256) < 1e-6f);
    p.findChannel(t1.channelId)->mute = false;

    // solo another track -> t1 silent
    auto& t2 = addTrack(p, TrackType::Audio, "Other");
    p.findChannel(t2.channelId)->solo = true;
    REQUIRE(rt.rebuild(p));
    out = render(engine, 24000);
    CHECK(peak(out[0], 256) < 1e-6f);
    // soloing the bus that t1 feeds makes it audible again (solo-in-place)
    p.findChannel(vocalsBus)->solo = true;
    rt.syncParams(p);
    out = render(engine, 24000);
    CHECK(peak(out[0], 512) > 0.2f);
}

TEST_CASE("gmb01", "pre-fader send ignores fader") {
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeSine(48000.0, 200.0, 1.0, 0.5f));
    auto& t1 = addTrack(p, TrackType::Audio, "Lead");
    addClip(t1, id, 0, 2);
    auto* ch = p.findChannel(t1.channelId);
    ch->gainDb = -120.0f; // fader fully down
    Send s;
    s.id = files::newId();
    s.targetChannelId = p.channels[4].id;
    s.levelDb = 0.0f;
    s.preFader = true;
    ch->sends.push_back(s);
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 24000);
    CHECK_NEAR(peak(out[0]), 0.5, 0.01);
}

TEST_CASE("gmb01", "plugin delay compensation aligns paths") {
    registerTestProcessors();
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeImpulse(48000.0, 0.5, 1000, 0.5f));
    const std::string aId = addTrack(p, TrackType::Audio, "A").id;
    const std::string bId = addTrack(p, TrackType::Audio, "B").id;
    addClip(*p.findTrack(aId), id, 0, 1);
    addClip(*p.findTrack(bId), id, 0, 1);
    PluginSlot slot;
    slot.id = files::newId();
    slot.typeId = "test.latent100";
    p.findChannel(p.findTrack(aId)->channelId)->inserts.push_back(slot);
    REQUIRE(rt.rebuild(p));
    CHECK(rt.graphLatencySamples() == 100);
    auto out = render(engine, 4000);
    // Both impulses must arrive together (sum = 1.0) at 1000 + 100.
    CHECK(argmaxAbs(out[0]) == 1100);
    CHECK_NEAR(peak(out[0]), 1.0, 1e-5);
}

TEST_CASE("gmb01", "automation drives channel gain") {
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeSine(48000.0, 300.0, 2.0, 0.5f));
    auto& t = addTrack(p, TrackType::Audio, "A");
    addClip(t, id, 0, 4);
    AutomationLane lane;
    lane.id = files::newId();
    lane.channelId = t.channelId;
    lane.paramId = "gain";
    lane.points = {{0.0, -60.0f}, {2.0, -60.0f}, {2.01, 0.0f}, {4.0, 0.0f}};
    p.automation.push_back(lane);
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 96000);
    CHECK(peak(out[0], 2000, 20000) < 0.01f);
    CHECK(peak(out[0], 60000, 90000) > 0.45f);
}

TEST_CASE("gmb01", "audio callback does not allocate") {
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeSine(48000.0, 440.0, 2.0));
    for (int i = 0; i < 8; ++i) {
        auto& t = addTrack(p, TrackType::Audio, "T");
        addClip(t, id, i * 0.1, 3);
        Send s;
        s.id = files::newId();
        s.targetChannelId = p.channels[4].id;
        p.findChannel(t.channelId)->sends.push_back(s);
    }
    p.loop = {true, 0.0, 1.0};
    REQUIRE(rt.rebuild(p));
    engine.transport().setLoop(true, 0, 24000);
    engine.metronome().setEnabled(true);
    engine.transport().play();
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    engine.process(nullptr, 0, outs, 2, 256); // warm-up
    long allocations = 0;
    {
        AllocationCounter counter;
        for (int i = 0; i < 400; ++i) engine.process(nullptr, 0, outs, 2, 256);
        allocations = counter.count();
    }
    CHECK_MSG(allocations == 0, std::format("{} allocations in audio callback", allocations));
    CHECK(engine.transport().loopCount() > 0);
}

TEST_CASE("gmb01", "non-finite samples never reach the output") {
    registerTestProcessors();
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    PluginSlot slot;
    slot.id = files::newId();
    slot.typeId = "test.nan";
    p.master()->inserts.push_back(slot);
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 4096);
    CHECK(allFinite(out[0]));
    CHECK(allFinite(out[1]));
    CHECK(engine.stats().nonFiniteFixes > 0);
}

TEST_CASE("gmb01", "graph hot swap while playing") {
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("t");
    auto id = addMemoryAsset(p, rt, makeSine(48000.0, 440.0, 2.0));
    auto& t = addTrack(p, TrackType::Audio, "A");
    addClip(t, id, 0, 4);
    REQUIRE(rt.rebuild(p));
    engine.transport().play();
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    for (int i = 0; i < 50; ++i) {
        engine.process(nullptr, 0, outs, 2, 256);
        if (i % 5 == 0) {
            p.findChannel(t.channelId)->gainDb = static_cast<float>(-i % 12);
            REQUIRE(rt.rebuild(p));
        }
    }
    engine.collectGarbage();
    CHECK(engine.processedBlocks() >= 50);
}

TEST_CASE("gmb01", "null audio device runs the engine") {
    AudioEngine engine;
    DeviceManager dm;
    std::string err;
    REQUIRE(dm.initialise("null", &err));
    AudioDeviceConfig cfg;
    cfg.backend = "null";
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    REQUIRE(dm.open(cfg, engine, &err));
    CHECK(engine.sampleRate() == 48000.0);
    REQUIRE(dm.start(&err));
    const auto t0 = std::chrono::steady_clock::now();
    while (dm.callbackCount() < 5 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dm.stop();
    CHECK(dm.callbackCount() >= 5);
    CHECK(engine.processedBlocks() >= 5);
    CHECK(dm.latencySamples() > 0);
    dm.close();
}
