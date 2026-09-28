// Multi-core mixing: output must be bit-identical for any number of worker threads,
// including busses, sends, sidechains and plugin delay compensation.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

Project buildProject(ProjectRuntime& rt) {
    Project p = makeNewProject("parallel", SR, 128.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt};
    auto run = [&](const std::string& id, const json& a) {
        REQUIRE(reg.execute(ctx, id, a));
        return ctx.result;
    };
    const std::string asset = addMemoryAsset(p, rt, makeSine(SR, 330.0, 4.0, 0.3f));
    const std::string bus = run("AddBus", {{"name", "Verb"}}).value("id", "");
    run("AddInsert", {{"channelId", bus}, {"typeId", "roy.reverb"}});
    run("AddPattern", {{"name", "P"}});
    const std::string pat = ctx.result.value("id", "");
    run("SetRowPattern", {{"patternId", pat}, {"voice", "kick"}, {"text", "x...x...x...x..."}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "closed_hat"}, {"text", "xxxxxxxxxxxxxxxx"}});
    std::string kickCh;
    for (int i = 0; i < 4; ++i) {
        auto a = run("AddTrack", {{"type", "audio"}, {"name", "A"}});
        run("AddAudioClip", {{"trackId", a.value("id", "")}, {"assetId", asset}, {"startBeat", 0.5 * i}, {"lengthBeats", 6.0}});
        run("AddInsert", {{"channelId", a.value("channelId", "")}, {"typeId", i == 0 ? "roy.limiter" : "roy.eq"}}); // limiter = latency (PDC)
        run("AddSend", {{"channelId", a.value("channelId", "")}, {"target", bus}, {"levelDb", -9.0}, {"preFader", i % 2 == 1}});
        auto m = run("AddTrack", {{"type", "midi"}, {"name", "M"}});
        run("AddMidiClip", {{"trackId", m.value("id", "")}, {"lengthBeats", 8.0}});
        const std::string clip = ctx.result.value("id", "");
        for (int n : {57, 60, 64}) run("AddNote", {{"clipId", clip}, {"pitch", n + i}, {"startBeat", 0.25 * i}, {"lengthBeats", 3.0}, {"wrongNoteMode", "off"}});
        auto d = run("AddTrack", {{"type", "beat"}, {"name", "D"}});
        run("AddPatternClip", {{"trackId", d.value("id", "")}, {"patternId", pat}, {"lengthBeats", 8.0}});
        if (i == 0) kickCh = d.value("channelId", "");
        else run("AddInsert", {{"channelId", m.value("channelId", "")}, {"typeId", "roy.compressor"}, {"sidechain", kickCh}}); // sidechain duck
    }
    run("CreateMasterChain", {{"preset", "club"}});
    return p;
}

// The SAME project (same ids -> same graph order) rendered by a fresh engine per worker count.
struct Fixture {
    AudioEngine engine0;
    ProjectRuntime rt0{engine0};
    Project p;
    std::vector<std::pair<std::string, std::shared_ptr<const AudioData>>> assets;
    Fixture() {
        registerBuiltinProcessors();
        engine0.prepare(SR, 256);
        p = buildProject(rt0);
        for (auto& a : p.assets) assets.push_back({a.id, rt0.asset(p, a.id)});
    }
    std::vector<std::vector<float>> render(int workers, int block) {
        AudioEngine engine;
        engine.prepare(SR, block);
        engine.setWorkerThreads(workers);
        ProjectRuntime rt(engine);
        for (auto& [id, d] : assets) rt.addLoadedAsset(id, d);
        REQUIRE(rt.rebuild(p));
        auto out = roytest::render(engine, static_cast<int64_t>(3.0 * SR), block);
        engine.setWorkerThreads(0);
        return out;
    }
};
} // namespace

TEST_CASE("parallel", "multi-core mixing is bit-identical to single-threaded") {
    Fixture f;
    const auto ref = f.render(0, 256);
    REQUIRE(ref.size() == 2);
    CHECK(rms(ref[0]) > 0.01);
    CHECK(f.render(0, 256)[0] == ref[0]); // deterministic at all
    for (int workers : {1, 3, 7}) {
        const auto par = f.render(workers, 256);
        CHECK_MSG(par[0] == ref[0] && par[1] == ref[1], std::format("{} workers differ", workers));
    }
}

TEST_CASE("parallel", "graph levels respect routing and sidechains") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = buildProject(rt);
    REQUIRE(rt.rebuild(p));
    const RenderGraph* g = engine.currentGraphForMessageThread();
    REQUIRE(g != nullptr);
    REQUIRE(g->levelStart.size() >= 3);
    std::vector<int> levelOf(g->channels.size(), -1);
    for (size_t lv = 0; lv + 1 < g->levelStart.size(); ++lv)
        for (int k = g->levelStart[lv]; k < g->levelStart[lv + 1]; ++k) levelOf[static_cast<size_t>(g->levelOrder[static_cast<size_t>(k)])] = static_cast<int>(lv);
    for (size_t i = 0; i < g->channels.size(); ++i) {
        CHECK(levelOf[i] >= 0);
        for (auto& in : g->channels[i].incoming) CHECK(levelOf[static_cast<size_t>(in.source)] < levelOf[i]);
        for (auto& ins : g->channels[i].inserts)
            if (ins.sidechainChannel >= 0) CHECK(levelOf[static_cast<size_t>(ins.sidechainChannel)] < levelOf[i]);
    }
    CHECK(levelOf[static_cast<size_t>(g->master)] == static_cast<int>(g->levelStart.size()) - 2); // master last
    // first level holds all 12 track channels -> parallel work
    CHECK(g->levelStart[1] - g->levelStart[0] >= 12);
}

TEST_CASE("parallel", "worker threads start and stop cleanly while graphs change") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 128);
    ProjectRuntime rt(engine);
    Project p = buildProject(rt);
    for (int round = 0; round < 5; ++round) {
        engine.setWorkerThreads(round % 3 + 1);
        REQUIRE(rt.rebuild(p));
        auto out = render(engine, 4800, 128);
        CHECK(allFinite(out[0]));
        engine.setWorkerThreads(0);
    }
    CHECK(engine.workerThreads() == 0);
}
