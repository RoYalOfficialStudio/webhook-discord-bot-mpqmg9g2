// AUTOMATION: curve shapes (Linear / Hold / Smooth / Bezier), volume, pan, send and tempo
// automation through the command system, undo, save/reload, old-format compatibility,
// and the O(log n) tempo map that dense tempo automation relies on.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "project/Automation.h"
#include "project/AutomationShape.h"
#include "project/ProjectIO.h"

#include <chrono>
#include <random>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

struct Proj {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("automation", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string trackId, channelId;
    Proj() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        // constant-amplitude source: DC-free sine, measured by block peak
        const std::string asset = addMemoryAsset(p, rt, makeSine(SR, 1000.0, 20.0, 0.5f));
        Track& t = addTrack(p, TrackType::Audio, "Sine");
        trackId = t.id;
        channelId = t.channelId;
        addClip(*p.findTrack(trackId), asset, 0, 32.0);
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s failed: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    std::vector<std::vector<float>> render(int64_t frames) {
        REQUIRE(rt.rebuild(p));
        engine.transport().seek(0);
        engine.transport().play();
        return roytest::render(engine, frames, 256);
    }
};
// peak around a time (seconds) in a 10 ms window
float peakAt(const std::vector<float>& v, double seconds) {
    const size_t c = static_cast<size_t>(seconds * SR);
    return peak(v, c, std::min(v.size(), c + 480));
}
} // namespace

TEST_CASE("automation", "curve shapes: linear, hold, smooth, bezier") {
    using namespace automation;
    CHECK_NEAR(shape(Linear, 0, 0.25), 0.25, 1e-12);
    CHECK_NEAR(shape(Hold, 0, 0.99), 0.0, 1e-12);
    CHECK_NEAR(shape(Smooth, 0, 0.5), 0.5, 1e-12);
    CHECK(shape(Smooth, 0, 0.1) < 0.1);             // eases in
    CHECK(shape(Smooth, 0, 0.9) > 0.9);             // eases out
    CHECK_NEAR(shape(Bezier, 0, 0.3), 0.3, 1e-9);   // no tension = linear
    CHECK(shape(Bezier, 0.8, 0.3) > 0.3);           // fast start
    CHECK(shape(Bezier, -0.8, 0.3) < 0.3);          // slow start
    for (int c : {Linear, Smooth, Bezier})
        for (double t : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            double prev = -1;
            bool mono = true;
            for (int i = 0; i <= 1000; ++i) {
                const double y = shape(c, t, i / 1000.0);
                mono &= y >= prev - 1e-12 && y >= -1e-12 && y <= 1 + 1e-12;
                prev = y;
            }
            CHECK_MSG(mono, std::format("{} tension {}", curveName(c), t));
            CHECK_NEAR(shape(c, t, 0.0), 0.0, 1e-9);
            CHECK_NEAR(shape(c, t, 1.0), 1.0, 1e-9);
        }
}

TEST_CASE("automation", "volume automation: Hold steps, Linear ramps, audio follows") {
    Proj s;
    const float ref = peak(s.render(SR)[0], 4800);
    // Hold: 0 dB until beat 4 (2 s), then -20 dB
    REQUIRE(s.run("CreateAutomation", {{"channelId", s.channelId}, {"paramId", "gain"},
                                        {"points", {{0.0, 0.0, 1, 0.0}, {4.0, -20.0}}}}));
    auto out = s.render(static_cast<int64_t>(3 * SR));
    CHECK_NEAR(peakAt(out[0], 1.9) / ref, 1.0, 0.02);
    CHECK_NEAR(peakAt(out[0], 2.5) / ref, 0.1, 0.01);
    // switch the lane to Linear: halfway (1 s = beat 2) = -10 dB
    const std::string lane = s.p.automation.back().id;
    REQUIRE(s.run("SetAutomationCurve", {{"laneId", lane}, {"curve", "Linear"}}));
    out = s.render(static_cast<int64_t>(3 * SR));
    CHECK_NEAR(peakAt(out[0], 1.0) / ref, std::pow(10.0, -10.0 / 20.0), 0.03);
    // Smooth: halfway still -10 dB, quarter way is closer to 0 dB than linear
    REQUIRE(s.run("SetAutomationCurve", {{"laneId", lane}, {"curve", "Smooth"}}));
    out = s.render(static_cast<int64_t>(3 * SR));
    CHECK_NEAR(peakAt(out[0], 1.0) / ref, std::pow(10.0, -10.0 / 20.0), 0.03);
    CHECK(peakAt(out[0], 0.5) / ref > std::pow(10.0, -5.0 / 20.0));
    // Bezier with negative tension stays higher for longer
    REQUIRE(s.run("SetAutomationCurve", {{"laneId", lane}, {"curve", "Bezier"}, {"tension", -0.9}}));
    out = s.render(static_cast<int64_t>(3 * SR));
    CHECK(peakAt(out[0], 1.0) / ref > std::pow(10.0, -8.0 / 20.0));
    // point editing and undo
    REQUIRE(s.run("SetAutomationPoint", {{"laneId", lane}, {"index", 1}, {"value", -6.0}, {"curve", "Hold"}}));
    CHECK(s.p.automation.back().points[1].value == -6.0f);
    REQUIRE(s.undo.undo());
    CHECK(s.p.automation.back().points[1].value == -20.0f);
    REQUIRE(s.run("DeleteAutomationPoint", {{"laneId", lane}, {"index", 0}}));
    CHECK(s.p.automation.back().points.size() == 1);
    CHECK(!s.run("DeleteAutomationPoint", {{"laneId", lane}, {"index", 7}}));
}

TEST_CASE("automation", "pan and send automation") {
    Proj s;
    // pan: hard left -> hard right over 4 beats
    REQUIRE(s.run("CreateAutomation", {{"channelId", s.channelId}, {"paramId", "pan"}, {"points", {{0.0, -1.0}, {4.0, 1.0}}}}));
    auto out = s.render(static_cast<int64_t>(2.5 * SR));
    CHECK(peakAt(out[0], 0.05) > 10 * peakAt(out[1], 0.05) + 1e-4);
    CHECK(peakAt(out[1], 2.2) > 10 * peakAt(out[0], 2.2) + 1e-4);
    REQUIRE(s.undo.undo());

    // send: the direct path is muted by the fader, a pre-fader send feeds a bus
    REQUIRE(s.run("AddBus", {{"name", "FX"}}));
    const std::string bus = s.ctx->result["id"];
    REQUIRE(s.run("AddSend", {{"channelId", s.channelId}, {"target", bus}, {"levelDb", 0.0}, {"preFader", true}}));
    const std::string send = s.ctx->result["id"];
    REQUIRE(s.run("SetChannelGain", {{"channelId", s.channelId}, {"gainDb", -120.0}}));
    CHECK(!s.run("CreateAutomation", {{"channelId", s.channelId}, {"paramId", "send:nope"}}));
    REQUIRE(s.run("CreateAutomation", {{"channelId", s.channelId}, {"paramId", "send:" + send},
                                        {"points", {{0.0, -60.0, 1, 0.0}, {2.0, 0.0}}}}));
    out = s.render(static_cast<int64_t>(2 * SR));
    const float quiet = peakAt(out[0], 0.5), loud = peakAt(out[0], 1.5);
    CHECK(quiet < 0.01f);
    CHECK(loud > 0.1f);
    // save / reload keeps the lane and its curve
    const fs::path dir = tempDir("automation_send");
    REQUIRE(saveProject(s.p, dir / "a.roy").ok);
    Project q;
    REQUIRE(loadProject(dir / "a.roy", q).ok);
    REQUIRE(q.automation.size() == 1);
    CHECK(q.automation[0].paramId == "send:" + send);
    CHECK(q.automation[0].points[0].curve == automation::Hold);
}

TEST_CASE("automation", "tempo automation: ramps change the timeline, Hold keeps it, undo restores") {
    Proj s;
    const std::string master = s.p.master()->id;
    CHECK(!s.run("CreateAutomation", {{"channelId", s.channelId}, {"paramId", "tempo"}})); // master only
    REQUIRE(s.run("CreateAutomation", {{"channelId", master}, {"paramId", "tempo"}, {"points", {{0.0, 120.0}, {8.0, 180.0}}}}));
    const std::string lane = s.ctx->result["id"];
    CHECK(!s.run("CreateAutomation", {{"channelId", master}, {"paramId", "tempo"}})); // only one tempo lane
    // linear 120 -> 180 bpm over 8 beats: t = 60/7.5 * ln(180/120) s
    const double expect = 8.0 * std::log(1.5);
    CHECK_NEAR(s.p.tempo.beatToSeconds(8.0), expect, 0.005);
    CHECK_NEAR(s.p.tempo.tempoAt(4.0), 150.0, 1.0);
    CHECK_NEAR(s.p.tempo.tempoAt(20.0), 180.0, 1e-9);
    CHECK_NEAR(s.p.tempo.secondsToBeat(s.p.tempo.beatToSeconds(5.3)), 5.3, 1e-9);
    // a clip at beat 8 starts where the ramp says
    REQUIRE(s.rt.rebuild(s.p));
    CHECK_NEAR(s.p.tempo.beatToSample(8.0, SR), expect * SR, 0.005 * SR);
    // Hold keeps 120 until beat 8
    REQUIRE(s.run("SetAutomationCurve", {{"laneId", lane}, {"curve", "Hold"}}));
    CHECK_NEAR(s.p.tempo.beatToSeconds(8.0), 4.0, 1e-9);
    CHECK_NEAR(s.p.tempo.tempoAt(9.0), 180.0, 1e-9);
    // undo brings the ramp back, deleting the lane leaves the start tempo
    REQUIRE(s.undo.undo());
    CHECK_NEAR(s.p.tempo.beatToSeconds(8.0), expect, 0.005);
    REQUIRE(s.run("DeleteAutomation", {{"laneId", lane}}));
    CHECK(s.p.tempo.tempoEvents().size() == 1);
    CHECK_NEAR(s.p.tempo.tempoAt(10.0), 120.0, 1e-9);
    // the engine plays through a tempo ramp without problems
    REQUIRE(s.run("CreateAutomation", {{"channelId", master}, {"paramId", "tempo"}, {"points", {{0.0, 90.0, 2, 0.0}, {16.0, 160.0}}}}));
    auto out = s.render(static_cast<int64_t>(6 * SR));
    CHECK(allFinite(out[0]));
    CHECK(peak(out[0]) > 0.1f);
}

TEST_CASE("automation", "old projects with [beat, value] points load as Linear; bad points are dropped") {
    Project p = makeNewProject("old", SR, 120.0);
    Track& t = addTrack(p, TrackType::Audio, "A");
    json j = projectToJson(p);
    j["automation"] = json::array({{{"id", "l1"}, {"channelId", t.channelId}, {"slotId", ""}, {"paramId", "gain"}, {"enabled", true},
                                    {"points", json::array({json::array({0.0, -3.0}), json::array({4.0, 0.0, 3, 0.5}),
                                                            json::array({"x", 1}), json::array({1.0})})}}});
    Project q;
    std::string err;
    REQUIRE_MSG_OK(projectFromJson(j, q, &err), err);
    REQUIRE(q.automation.size() == 1);
    REQUIRE(q.automation[0].points.size() == 2);
    CHECK(q.automation[0].points[0].curve == automation::Linear);
    CHECK(q.automation[0].points[1].curve == automation::Bezier);
    CHECK_NEAR(q.automation[0].points[1].tension, 0.5, 1e-6);
    // Linear points keep the compact v1 layout on save (older builds can still read them)
    json back = projectToJson(q);
    CHECK(back["automation"][0]["points"][0].size() == 2);
    CHECK(back["automation"][0]["points"][1].size() == 4);
}

TEST_CASE("automation", "tempo map with thousands of events: fast and exact") {
    TempoMap m;
    std::vector<TempoEvent> ev;
    std::mt19937 rng(7);
    for (int i = 0; i < 10000; ++i) ev.push_back({i * 0.125, 60.0 + (rng() % 1200) / 10.0});
    m.setTempoEvents(ev);
    // reference: sequential sum
    auto slowSeconds = [&](double beat) {
        double sec = 0;
        const auto& e = m.tempoEvents();
        for (size_t i = 0; i < e.size(); ++i) {
            const double end = i + 1 < e.size() ? e[i + 1].beat : beat;
            if (beat <= e[i].beat) break;
            sec += (std::min(beat, end) - e[i].beat) * 60.0 / e[i].bpm;
            if (beat <= end) break;
        }
        return sec;
    };
    for (double b : {0.01, 3.3, 100.0, 777.7, 1249.99, 1300.0}) {
        CHECK_NEAR(m.beatToSeconds(b), slowSeconds(b), 1e-9);
        CHECK_NEAR(m.secondsToBeat(m.beatToSeconds(b)), b, 1e-9);
    }
    const auto t0 = std::chrono::steady_clock::now();
    double acc = 0;
    for (int i = 0; i < 1000000; ++i) acc += m.beatToSeconds((i % 12500) * 0.1) + m.tempoAt(i % 1250);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(acc > 0);
    CHECK_MSG(secs < 1.5, std::format("{:.3f} s for 2M lookups", secs));
}
