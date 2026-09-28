// MIXER HARDENING: solo safe, pre/post sends that switch live, send removal, bus deletion
// with re-routing, rename, and randomized routing graphs (busses, sends, pre/post)
// checked for superposition, cycle rejection and multi-core bit identity.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"

#include <random>

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

struct Mix {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("mixer", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    Mix() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
    }
    std::string track(const std::string& name, double freq, float amp) {
        const std::string asset = addMemoryAsset(p, rt, makeSine(SR, freq, 3.0, amp), name);
        Track& t = addTrack(p, TrackType::Audio, name);
        addClip(t, asset, 0, 6.0);
        return t.channelId;
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s failed: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    std::vector<std::vector<float>> render(int64_t frames = 9600) {
        REQUIRE(rt.rebuild(p));
        engine.transport().seek(0);
        engine.transport().play();
        return roytest::render(engine, frames, 256);
    }
};
float level(const std::vector<std::vector<float>>& o) { return peak(o[0], 2400) + peak(o[1], 2400); }
} // namespace

TEST_CASE("mixer", "solo safe: a solo-safe track stays audible while another track is soloed") {
    Mix m;
    const std::string a = m.track("A", 220, 0.3f), b = m.track("B", 330, 0.3f), ref = m.track("Ref", 440, 0.3f);
    REQUIRE(m.run("SoloChannel", {{"channelId", a}, {"solo", true}}));
    const float onlyA = level(m.render());
    REQUIRE(m.run("SetSoloSafe", {{"channelId", ref}, {"safe", true}}));
    const float aAndRef = level(m.render());
    CHECK(aAndRef > onlyA * 1.3f);
    CHECK(!m.run("SetSoloSafe", {{"channelId", m.p.master()->id}, {"safe", true}}));
    // explicit mute still wins over solo safe
    REQUIRE(m.run("MuteChannel", {{"channelId", ref}, {"mute", true}}));
    CHECK_NEAR(level(m.render()), onlyA, 1e-4f);
    REQUIRE(m.undo.undo());
    REQUIRE(m.undo.undo());
    CHECK(!m.p.findChannel(ref)->soloSafe);
    (void)b;
}

TEST_CASE("mixer", "sends: pre/post switch takes effect immediately, remove send drops its automation") {
    Mix m;
    const std::string a = m.track("A", 220, 0.5f);
    REQUIRE(m.run("AddBus", {{"name", "FX"}}));
    const std::string bus = m.ctx->result["id"];
    REQUIRE(m.run("AddSend", {{"channelId", a}, {"target", bus}, {"levelDb", 0.0}}));
    const std::string send = m.ctx->result["id"];
    REQUIRE(m.run("SetChannelGain", {{"channelId", a}, {"gainDb", -120.0}}));
    CHECK(level(m.render()) < 1e-3f);                      // post-fader: the fader silences the send too
    REQUIRE(m.run("SetSend", {{"sendId", send}, {"preFader", true}}));
    CHECK(level(m.render()) > 0.3f);                       // pre-fader: independent of the fader
    REQUIRE(m.run("SetSend", {{"sendId", send}, {"enabled", false}}));
    CHECK(level(m.render()) < 1e-3f);
    REQUIRE(m.run("SetSend", {{"sendId", send}, {"enabled", true}}));
    REQUIRE(m.run("CreateAutomation", {{"channelId", a}, {"paramId", "send:" + send}, {"points", {{0.0, 0.0}}}}));
    REQUIRE(m.run("RemoveSend", {{"sendId", send}}));
    CHECK(m.p.findChannel(a)->sends.empty());
    CHECK(m.p.automation.empty());
    CHECK(level(m.render()) < 1e-3f);
    CHECK(m.rt.lastWarnings().empty());
    REQUIRE(m.undo.undo());
    CHECK(m.p.findChannel(a)->sends.size() == 1);
    CHECK(m.p.automation.size() == 1);
}

TEST_CASE("mixer", "delete bus re-routes its inputs, removes sends into it; rename keeps track and channel in sync") {
    Mix m;
    const std::string a = m.track("A", 220, 0.5f), b = m.track("B", 330, 0.5f);
    REQUIRE(m.run("AddBus", {{"name", "Drums"}}));
    const std::string drums = m.ctx->result["id"];
    REQUIRE(m.run("AddBus", {{"name", "Group"}}));
    const std::string group = m.ctx->result["id"];
    REQUIRE(m.run("RouteChannel", {{"channelId", drums}, {"output", group}}));
    REQUIRE(m.run("RouteChannel", {{"channelId", a}, {"output", drums}}));
    REQUIRE(m.run("AddSend", {{"channelId", b}, {"target", drums}, {"levelDb", -6.0}}));
    REQUIRE(m.run("CreateAutomation", {{"channelId", drums}, {"paramId", "gain"}, {"points", {{0.0, -3.0}}}}));
    const float before = level(m.render());
    REQUIRE(m.run("DeleteBus", {{"channelId", drums}}));
    CHECK(m.p.findChannel(drums) == nullptr);
    CHECK(m.p.findChannel(a)->outputChannelId == group); // A still reaches the mix
    CHECK(m.p.findChannel(b)->sends.empty());
    CHECK(m.p.automation.empty());
    CHECK(level(m.render()) > 0.3f);
    CHECK(!m.run("DeleteBus", {{"channelId", a}})); // tracks are deleted with DeleteTrack
    REQUIRE(m.undo.undo());
    CHECK_NEAR(level(m.render()), before, 1e-5f);
    // rename
    REQUIRE(m.run("RenameChannel", {{"channelId", a}, {"name", "Kick"}}));
    CHECK(m.p.findChannel(a)->name == "Kick");
    bool trackRenamed = false;
    for (auto& t : m.p.tracks) trackRenamed |= t.channelId == a && t.name == "Kick";
    CHECK(trackRenamed);
    CHECK(!m.run("RenameChannel", {{"channelId", a}, {"name", ""}}));
}

TEST_CASE("mixer", "random routing graphs: cycles rejected, superposition holds, multi-core is bit-identical") {
    std::mt19937 rng(20260928);
    for (int round = 0; round < 12; ++round) {
        Mix m;
        std::vector<std::string> tracks, busses;
        const int nT = 3 + static_cast<int>(rng() % 5), nB = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < nT; ++i) tracks.push_back(m.track(std::format("T{}", i), 100.0 + 37.0 * i, 0.2f));
        for (int i = 0; i < nB; ++i) {
            REQUIRE(m.run("AddBus", {{"name", std::format("B{}", i)}}));
            busses.push_back(m.ctx->result["id"]);
        }
        // bus i routes to a later bus or the master: always a DAG
        for (int i = 0; i < nB; ++i) {
            const int to = i + 1 + static_cast<int>(rng() % static_cast<unsigned>(nB - i));
            if (to < nB) REQUIRE(m.run("RouteChannel", {{"channelId", busses[i]}, {"output", busses[to]}}));
        }
        // every backwards route must be refused (it would close a loop)
        for (int i = 1; i < nB; ++i) {
            const MixerChannel* cur = m.p.findChannel(busses[i]);
            (void)cur;
            bool reachesI = false; // does busses[0]'s chain reach busses[i]?
            const MixerChannel* c = m.p.findChannel(busses[0]);
            for (int g = 0; c && g < 16; ++g) {
                if (c->id == busses[i]) reachesI = true;
                c = c->outputChannelId.empty() ? nullptr : m.p.findChannel(c->outputChannelId);
            }
            if (reachesI) CHECK(!m.run("RouteChannel", {{"channelId", busses[i]}, {"output", busses[0]}}));
        }
        for (auto& t : tracks) {
            if (rng() % 2) REQUIRE(m.run("RouteChannel", {{"channelId", t}, {"output", busses[rng() % busses.size()]}}));
            REQUIRE(m.run("SetChannelGain", {{"channelId", t}, {"gainDb", -static_cast<double>(rng() % 12)}}));
            REQUIRE(m.run("SetChannelPan", {{"channelId", t}, {"pan", (static_cast<int>(rng() % 21) - 10) / 10.0}}));
            const int sends = static_cast<int>(rng() % 3);
            for (int s = 0; s < sends; ++s)
                REQUIRE(m.run("AddSend", {{"channelId", t}, {"target", busses[rng() % busses.size()]},
                                          {"levelDb", -static_cast<double>(rng() % 18)}, {"preFader", rng() % 2 == 0}}));
        }
        for (auto& b : busses) REQUIRE(m.run("SetChannelGain", {{"channelId", b}, {"gainDb", -static_cast<double>(rng() % 6)}}));
        const auto full = m.render();
        CHECK(allFinite(full[0]) && allFinite(full[1]));
        // superposition: the mix equals the sum of each track rendered alone
        std::vector<std::vector<float>> sum(2, std::vector<float>(full[0].size(), 0.0f));
        for (auto& solo : tracks) {
            for (auto& t : tracks) m.p.findChannel(t)->mute = t != solo;
            const auto one = m.render();
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < sum[c].size(); ++i) sum[c][i] += one[c][i];
        }
        for (auto& t : tracks) m.p.findChannel(t)->mute = false;
        double err = 0;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < sum[c].size(); ++i) err = std::max(err, static_cast<double>(std::fabs(sum[c][i] - full[c][i])));
        CHECK_MSG(err < 1e-5, std::format("round {} superposition error {}", round, err));
        // multi-core rendering is bit-identical
        m.engine.setWorkerThreads(3);
        const auto par = m.render();
        m.engine.setWorkerThreads(0);
        CHECK_MSG(par[0] == full[0] && par[1] == full[1], std::format("round {}: parallel differs", round));
    }
}

TEST_CASE("mixer", "feedback loops through sends and sidechains are refused") {
    Mix m;
    const std::string a = m.track("A", 220, 0.5f);
    REQUIRE(m.run("AddBus", {{"name", "X"}}));
    const std::string x = m.ctx->result["id"];
    REQUIRE(m.run("AddBus", {{"name", "Y"}}));
    const std::string y = m.ctx->result["id"];
    REQUIRE(m.run("RouteChannel", {{"channelId", x}, {"output", y}}));
    CHECK(!m.run("AddSend", {{"channelId", y}, {"target", x}}));       // Y -> X would loop (X -> Y)
    REQUIRE(m.run("AddSend", {{"channelId", x}, {"target", y}}));      // parallel edge is fine
    REQUIRE(m.run("AddBus", {{"name", "Z"}}));
    const std::string z = m.ctx->result["id"];
    REQUIRE(m.run("AddSend", {{"channelId", y}, {"target", z}}));
    CHECK(!m.run("RouteChannel", {{"channelId", z}, {"output", x}}));  // loop via a send
    // sidechain: compressor on X keyed from Z, while Z is fed by X -> Y -> Z
    CHECK(!m.run("AddInsert", {{"channelId", x}, {"typeId", "roy.compressor"}, {"sidechain", z}}));
    REQUIRE(m.run("AddInsert", {{"channelId", x}, {"typeId", "roy.compressor"}, {"sidechain", a}}));
    const std::string comp = m.ctx->result["id"];
    CHECK(!m.run("SetSidechain", {{"slotId", comp}, {"sidechain", z}}));
    CHECK(!m.run("SetSidechain", {{"slotId", comp}, {"sidechain", "missing"}}));
    REQUIRE(m.run("SetSidechain", {{"slotId", comp}, {"sidechain", ""}}));
    CHECK(m.p.findSlot(comp)->sidechainChannelId.empty());
    auto out = m.render();
    CHECK(allFinite(out[0]));
    CHECK(m.rt.lastWarnings().empty()); // nothing had to be dropped by the runtime
}
