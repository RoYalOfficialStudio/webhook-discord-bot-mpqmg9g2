// MIDI LEARN: controller -> mixer / plugin parameter mappings. Commands (validation, one
// controller per target and one target per controller, undo), project save/load, applying CC
// values to model + running engine (audible), mapped CCs consumed by the input (not sent to the
// instrument), learn mode, and mappings removed together with their targets.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "midi/MidiInput.h"
#include "midi/MidiLearn.h"
#include "project/ProjectIO.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

struct Learn {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("learn", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string track, channel, eq;
    Learn() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        const std::string asset = addMemoryAsset(p, rt, makeSine(SR, 440, 3.0, 0.5f), "tone");
        Track& t = addTrack(p, TrackType::Audio, "Vocal");
        addClip(t, asset, 0, 6.0);
        track = t.id;
        channel = t.channelId;
        REQUIRE(run("AddInsert", {{"channelId", channel}, {"typeId", "roy.eq"}, {"name", "RoY EQ"}}));
        eq = ctx->result["id"];
        REQUIRE(rt.rebuild(p));
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
    float level() {
        engine.transport().seek(0);
        engine.transport().play();
        auto o = roytest::render(engine, 9600, 256);
        return peak(o[0], 2400);
    }
    MixerChannel& ch() { return *p.findChannel(channel); }
};
} // namespace

TEST_CASE("midi-learn", "mapping commands: validation, replace on same CC or target, undo, save/load") {
    Learn l;
    CHECK(!l.run("AddMidiMapping", {{"cc", 7}, {"channelId", l.channel}, {"paramId", "nope"}}));
    CHECK(!l.run("AddMidiMapping", {{"cc", 121}, {"channelId", l.channel}, {"paramId", "gain"}})); // channel mode message
    CHECK(!l.run("AddMidiMapping", {{"cc", 7}, {"channelId", "missing"}, {"paramId", "gain"}}));
    CHECK(!l.run("AddMidiMapping", {{"cc", 7}, {"slotId", l.eq}, {"paramId", "no_such_param"}}));
    CHECK(l.undo.undoCount() == 1); // only the fixture's AddInsert

    REQUIRE(l.run("AddMidiMapping", {{"cc", 7}, {"channel", 0}, {"channelId", l.channel}, {"paramId", "gain"}}));
    REQUIRE(l.p.midiMappings.size() == 1);
    CHECK_NEAR(l.p.midiMappings[0].minValue, -60.0f, 1e-6);
    CHECK_NEAR(l.p.midiMappings[0].maxValue, 6.0f, 1e-6);
    REQUIRE(l.run("AddMidiMapping", {{"cc", 10}, {"channel", 0}, {"channelId", l.channel}, {"paramId", "pan"}}));
    // the EQ's first parameter: owner channel is filled in, range = the parameter's range
    auto proc = l.rt.processorForSlot(l.eq);
    REQUIRE(proc && proc->numParams() > 0);
    const auto pi = proc->paramInfo(0);
    REQUIRE(l.run("AddMidiMapping", {{"cc", 21}, {"channel", 0}, {"slotId", l.eq}, {"paramId", pi.id}}));
    CHECK(l.p.midiMappings.back().channelId == l.channel);
    CHECK_NEAR(l.p.midiMappings.back().maxValue, pi.maxValue, 1e-6);
    CHECK(l.p.midiMappings.size() == 3);
    // same CC again -> the old mapping is replaced (one controller, one target)
    REQUIRE(l.run("AddMidiMapping", {{"cc", 10}, {"channel", 0}, {"channelId", l.channel}, {"paramId", "width"}}));
    CHECK(l.p.midiMappings.size() == 3);
    CHECK(midi::findMappingForTarget(l.p, l.channel, "", "pan") == nullptr);
    // same target again with a new CC -> replaced as well
    REQUIRE(l.run("AddMidiMapping", {{"cc", 11}, {"channel", 0}, {"channelId", l.channel}, {"paramId", "gain"}}));
    CHECK(l.p.midiMappings.size() == 3);
    CHECK(midi::findMappingForTarget(l.p, l.channel, "", "gain")->cc == 11);
    l.undo.undo();
    CHECK(midi::findMappingForTarget(l.p, l.channel, "", "gain")->cc == 7);
    // save / load keeps every field
    Project back;
    std::string err;
    REQUIRE(deserializeProject(serializeProject(l.p), back, &err));
    REQUIRE(back.midiMappings.size() == l.p.midiMappings.size());
    for (size_t i = 0; i < back.midiMappings.size(); ++i) {
        CHECK(back.midiMappings[i].id == l.p.midiMappings[i].id);
        CHECK(back.midiMappings[i].cc == l.p.midiMappings[i].cc);
        CHECK(back.midiMappings[i].channel == l.p.midiMappings[i].channel);
        CHECK(back.midiMappings[i].slotId == l.p.midiMappings[i].slotId);
        CHECK(back.midiMappings[i].paramId == l.p.midiMappings[i].paramId);
        CHECK_NEAR(back.midiMappings[i].maxValue, l.p.midiMappings[i].maxValue, 1e-6);
    }
    // remove by id and remove all
    REQUIRE(l.run("RemoveMidiMapping", {{"mappingId", l.p.midiMappings[0].id}}));
    CHECK(l.p.midiMappings.size() == 2);
    REQUIRE(l.run("ClearMidiMappings", json::object()));
    CHECK(l.p.midiMappings.empty());
    CHECK(!l.run("ClearMidiMappings", json::object()));
}

TEST_CASE("midi-learn", "controller moves change model + running engine audibly, last value wins, no undo steps") {
    Learn l;
    REQUIRE(l.run("AddMidiMapping", {{"cc", 7}, {"channel", 0}, {"channelId", l.channel}, {"paramId", "gain"}}));
    REQUIRE(l.run("AddMidiMapping", {{"cc", 10}, {"channelId", l.channel}, {"paramId", "pan"}})); // any channel
    const float unity = l.level();
    const size_t steps = l.undo.undoCount();
    // CC 7 = 0 -> -60 dB
    CHECK(midi::applyControls(l.p, &l.rt, {{0, 7, 0}}) == 1);
    CHECK_NEAR(l.ch().gainDb, -60.0f, 1e-4);
    CHECK(l.level() < unity * 0.002f);
    // several moves in one batch: the last one counts (127 -> +6 dB)
    CHECK(midi::applyControls(l.p, &l.rt, {{0, 7, 20}, {0, 7, 90}, {0, 7, 127}}) == 1);
    CHECK_NEAR(l.ch().gainDb, 6.0f, 1e-4);
    CHECK_NEAR(l.level() / unity, std::pow(10.0f, 6.0f / 20.0f), 0.02f);
    // wrong MIDI channel for a channel-specific mapping: ignored; "any" mapping matches channel 5
    CHECK(midi::applyControls(l.p, &l.rt, {{3, 7, 0}}) == 0);
    CHECK_NEAR(l.ch().gainDb, 6.0f, 1e-4);
    CHECK(midi::applyControls(l.p, &l.rt, {{5, 10, 0}}) == 1);
    CHECK_NEAR(l.ch().pan, -1.0f, 1e-4);
    // unmapped controller: nothing
    CHECK(midi::applyControls(l.p, &l.rt, {{0, 74, 64}}) == 0);
    CHECK(l.undo.undoCount() == steps);
    // processor parameter (stepped parameters are rounded by mappedValue)
    auto proc = l.rt.processorForSlot(l.eq);
    const auto pi = proc->paramInfo(0);
    REQUIRE(l.run("AddMidiMapping", {{"cc", 21}, {"channel", 0}, {"slotId", l.eq}, {"paramId", pi.id}}));
    CHECK(midi::applyControls(l.p, &l.rt, {{0, 21, 127}}) == 1);
    CHECK_NEAR(proc->getParam(0), pi.maxValue, 1e-4);
    CHECK_NEAR(l.p.findSlot(l.eq)->state["params"][pi.id].get<float>(), pi.maxValue, 1e-4);
    MidiMapping stepped;
    stepped.minValue = 0;
    stepped.maxValue = 4;
    CHECK(midi::mappedValue(stepped, 70, 5) == 2.0f);
    CHECK(midi::mappedValue(stepped, 70, 2) == 4.0f);
    CHECK(midi::mappedValue(stepped, 10, 2) == 0.0f);
    // the value survives a graph rebuild (it lives in the model)
    REQUIRE(l.rt.rebuild(l.p));
    CHECK_NEAR(l.rt.channelParams(l.channel)->gainDb.load(), 6.0f, 1e-4);
}

TEST_CASE("midi-learn", "input: mapped CCs reach the message thread but not the instrument; learn mode consumes all") {
    Learn l;
    midi::MidiInputManager in(l.engine);
    l.engine.setLiveMidiRecording(true);
    l.engine.transport().play(); // the engine records live MIDI only while rolling
    auto send = [&](std::initializer_list<uint8_t> b) {
        std::vector<uint8_t> v(b);
        in.inject(v.data(), v.size());
    };
    auto engineGot = [&]() {
        std::vector<float> a(256), b(256);
        float* outs[2] = {a.data(), b.data()};
        l.engine.process(nullptr, 0, outs, 2, 256);
        std::vector<RecordedMidi> rec;
        l.engine.drainRecordedMidi(rec);
        return rec;
    };
    in.setConsumedControls({{0, 7}, {-1, 10}});
    CHECK(in.isConsumed(0, 7));
    CHECK(!in.isConsumed(1, 7));
    CHECK(in.isConsumed(9, 10));
    send({0xB0, 7, 100, 0xB9, 10, 20, 0xB0, 74, 33, 0x90, 60, 100});
    const auto cc = in.drainControlChanges();
    REQUIRE(cc.size() == 3); // every CC is visible to the message thread (learn / mapped controls)
    CHECK((cc[0].channel == 0 && cc[0].cc == 7 && cc[0].value == 100));
    CHECK((cc[1].channel == 9 && cc[1].cc == 10));
    CHECK(in.drainControlChanges().empty());
    auto rec = engineGot();
    // the instrument only got the unmapped CC 74 and the note
    REQUIRE(rec.size() == 2);
    CHECK((rec[0].msg.status == 0xB0 && rec[0].msg.data1 == 74));
    CHECK(rec[1].msg.status == 0x90);
    // learn mode: every learnable CC is consumed, channel mode messages (123) still pass
    in.setLearning(true);
    send({0xB2, 1, 64, 0xB0, 123, 0});
    rec = engineGot();
    REQUIRE(rec.size() == 1);
    CHECK(rec[0].msg.data1 == 123);
    CHECK(in.drainControlChanges().size() == 1); // CC 123 is not learnable
    in.setLearning(false);
    send({0xB2, 1, 64});
    CHECK(engineGot().size() == 1);
    // bounded queue: nobody drains -> old controller moves are dropped, never unbounded growth
    for (int i = 0; i < 10000; ++i) send({0xB0, 74, static_cast<uint8_t>(i & 127)});
    CHECK(in.drainControlChanges().size() <= 4096);
}

TEST_CASE("midi-learn", "mappings go away with their targets (send, effect, track), undo restores them") {
    Learn l;
    REQUIRE(l.run("AddBus", {{"name", "FX"}}));
    const std::string bus = l.ctx->result["id"];
    REQUIRE(l.run("AddSend", {{"channelId", l.channel}, {"target", bus}, {"levelDb", -6.0}}));
    const std::string send = l.ctx->result["id"];
    const auto pi = l.rt.processorForSlot(l.eq)->paramInfo(0);
    REQUIRE(l.rt.rebuild(l.p));
    REQUIRE(l.run("AddMidiMapping", {{"cc", 1}, {"channelId", l.channel}, {"paramId", "send:" + send}}));
    REQUIRE(l.run("AddMidiMapping", {{"cc", 2}, {"slotId", l.eq}, {"paramId", pi.id}}));
    REQUIRE(l.run("AddMidiMapping", {{"cc", 3}, {"channelId", l.channel}, {"paramId", "gain"}}));
    REQUIRE(l.run("AddMidiMapping", {{"cc", 4}, {"channelId", bus}, {"paramId", "gain"}}));
    CHECK(midi::targetName(l.p, &l.rt, l.p.midiMappings[0]) == "Vocal · Send FX");
    CHECK(midi::targetName(l.p, &l.rt, l.p.midiMappings[2]) == "Vocal · Volume");
    // send level via CC
    CHECK(midi::applyControls(l.p, &l.rt, {{0, 1, 127}}) == 1);
    CHECK_NEAR(l.ch().sends[0].levelDb, 6.0f, 1e-4);
    REQUIRE(l.run("RemoveSend", {{"sendId", send}}));
    CHECK(l.p.midiMappings.size() == 3);
    REQUIRE(l.run("RemoveInsert", {{"slotId", l.eq}}));
    CHECK(l.p.midiMappings.size() == 2);
    REQUIRE(l.run("DeleteTrack", {{"trackId", l.track}}));
    CHECK(l.p.midiMappings.size() == 1); // only the bus volume
    REQUIRE(l.run("DeleteBus", {{"channelId", bus}}));
    CHECK(l.p.midiMappings.empty());
    for (int i = 0; i < 4; ++i) l.undo.undo();
    CHECK(l.p.midiMappings.size() == 4);
}
