// GMB 12 - Energy Map, section suggestions, Vocal DNA, Project Assistant, intelligence commands.
#include "TestFramework.h"
#include "TestHelpers.h"
#include "VoiceSynth.h"

#include "commands/Commands.h"
#include "intelligence/Arrangement.h"
#include "intelligence/Assistant.h"

#include <set>

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

// Synthetic song at 120 BPM (1 bar = 2 s): intro(4) verse(8) hook(8) verse(8) hook(8) outro(4).
struct Song {
    std::vector<float> mix, drums, vocal;
    std::vector<std::pair<std::string, int>> layout = {{"intro", 4}, {"verse", 8}, {"hook", 8}, {"verse", 8}, {"hook", 8}, {"outro", 4}};
    Song() {
        int bars = 0;
        for (auto& [n, b] : layout) bars += b;
        const size_t len = static_cast<size_t>(bars * 2.0 * SR);
        mix.assign(len, 0.0f);
        drums.assign(len, 0.0f);
        vocal.assign(len, 0.0f);
        Rng rng(5);
        size_t pos = 0;
        for (auto& [name, b] : layout) {
            const size_t secLen = static_cast<size_t>(b * 2.0 * SR);
            for (size_t i = 0; i < secLen; ++i) {
                const double t = static_cast<double>(i) / SR, beat = std::fmod(t, 0.5);
                float pad = static_cast<float>(0.05 * std::sin(kTwoPi * 220.0 * t));
                float d = 0, v = 0, bass = 0;
                if (name == "verse" || name == "hook") {
                    d = static_cast<float>(0.4 * std::exp(-beat / 0.06) * std::sin(kTwoPi * 60.0 * beat));
                    bass = static_cast<float>(0.15 * std::sin(kTwoPi * 55.0 * t));
                    v = static_cast<float>((std::fmod(t, 2.0) < 1.5 ? 0.12 : 0.0) * std::sin(kTwoPi * 330.0 * t));
                }
                if (name == "hook") {
                    d += static_cast<float>(0.15 * std::exp(-std::fmod(t, 0.25) / 0.02) * rng.uniform(-1, 1)); // hats
                    pad *= 4.0f;
                    v *= 1.8f;
                    bass *= 1.5f;
                }
                drums[pos + i] = d;
                vocal[pos + i] = v;
                mix[pos + i] = pad + d + v + bass;
            }
            pos += secLen;
        }
    }
};
} // namespace

TEST_CASE("gmb12", "energy map and section suggestions") {
    Song s;
    std::vector<mixi::TrackAudio> tracks = {{"d", "cd", "Drums", "drums", {s.drums, s.drums}}, {"v", "cv", "Vocal", "vocal", {s.vocal, s.vocal}}};
    TempoMap tempo(120.0);
    auto map = arrangei::buildEnergyMap({s.mix, s.mix}, tracks, SR, tempo);
    REQUIRE(map.bars.size() == 40);
    auto meanEnergy = [&](int a, int b) {
        double e = 0;
        for (int i = a; i < b; ++i) e += map.bars[static_cast<size_t>(i)].energy;
        return e / (b - a);
    };
    CHECK(meanEnergy(12, 20) > meanEnergy(4, 12));   // hook > verse
    CHECK(meanEnergy(4, 12) > meanEnergy(0, 4));     // verse > intro
    CHECK(map.bars[1].vocalActivity < 0.05);
    CHECK(map.bars[6].vocalActivity > 0.5);
    CHECK(map.bars[6].drumActivity > map.bars[1].drumActivity);
    CHECK(!map.note.empty());
    auto secs = arrangei::suggestSections(map, 4);
    REQUIRE(secs.size() >= 5);
    CHECK(secs.front().type == "intro");
    CHECK(secs.back().type == "outro");
    int hooks = 0;
    for (auto& x : secs)
        if (x.type == "hook") {
            ++hooks;
            const bool atHook = (std::fabs(x.startBeat - 48.0) <= 8.0) || (std::fabs(x.startBeat - 112.0) <= 8.0);
            CHECK_MSG(atHook, std::format("hook at beat {}", x.startBeat));
        }
    CHECK(hooks == 2);
}

TEST_CASE("gmb12", "vocal DNA learns the own voice and flags deviations") {
    assist::VocalDna dna;
    for (double base : {55.0, 57.0, 59.0}) {
        VoiceSpec v;
        v.seconds = 2.0;
        v.midi = [base](double t) { return std::fmod(t, 0.5) < 0.4 ? base + 2.0 * std::floor(t / 0.5) : 0.0; };
        assist::learnVocal(dna, {makeVoice(v)}, SR);
    }
    CHECK(dna.takes == 3);
    CHECK(dna.pitchLowMidi < 56.0);
    CHECK(dna.pitchHighMidi > 63.0 && dna.pitchHighMidi < 66.0);
    CHECK(dna.toJson()["note"].get<std::string>().find("not for voice imitation") != std::string::npos);
    auto round = assist::VocalDna::fromJson(dna.toJson());
    CHECK(round.takes == 3);
    VoiceSpec high;
    high.seconds = 1.5;
    high.midi = [](double t) { return std::fmod(t, 0.5) < 0.4 ? 72.0 : 0.0; };
    auto dev = assist::compareToDna(dna, {makeVoice(high)}, SR);
    bool hi = false;
    for (auto& d : dev) hi |= d.key == "pitch_high";
    CHECK(hi);
    VoiceSpec usual;
    usual.seconds = 1.5;
    usual.midi = [](double t) { return std::fmod(t, 0.5) < 0.4 ? 58.0 : 0.0; };
    for (auto& d : assist::compareToDna(dna, {makeVoice(usual)}, SR)) CHECK(d.key != "pitch_high");
}

TEST_CASE("gmb12", "project assistant findings") {
    registerBuiltinProcessors();
    auto dir = tempDir("assistant");
    Project p = makeNewProject("check");
    AudioAsset a;
    a.id = "a1";
    a.path = "Audio/missing.wav";
    a.originalName = "missing.wav";
    a.sampleRate = 44100;
    p.assets.push_back(a);
    const std::string t1 = addTrack(p, TrackType::Audio, "Empty").id;
    const std::string t2 = addTrack(p, TrackType::Audio, "Solo").id;
    p.findChannel(p.findTrack(t2)->channelId)->solo = true;
    p.findChannel(p.findTrack(t1)->channelId)->outputChannelId = "deleted-bus";
    PluginSlot s;
    s.id = "x";
    s.typeId = "vst3:unknown";
    p.master()->inserts.push_back(s);
    auto f = assist::checkProject(p, dir, true, 0);
    std::set<std::string> ids;
    for (auto& x : f) ids.insert(x.id);
    for (const char* want : {"missing_file", "unused_asset", "sample_rate", "solo_active", "empty_track", "bad_route", "missing_plugin",
                             "no_master_limiter", "unsaved", "no_backup"})
        CHECK_MSG(ids.count(want), want);
}

TEST_CASE("gmb12", "intelligence commands through the engine") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 512);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("intel", SR, 120.0);
    Song s;
    auto d = std::make_shared<AudioData>(AudioData{"", SR, 1, static_cast<int64_t>(s.mix.size()), {s.mix}});
    const std::string asset = addMemoryAsset(p, rt, d);
    const std::string tid = addTrack(p, TrackType::Audio, "Song").id;
    addClip(*p.findTrack(tid), asset, 0, 160.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt};
    REQUIRE(reg.execute(ctx, "EnergyMap", {{"startBeat", 0.0}, {"endBeat", 160.0}}));
    CHECK(ctx.result["bars"].size() == 40);
    auto sections = ctx.result["sections"];
    CHECK(sections.size() >= 5);
    REQUIRE(reg.execute(ctx, "ApplySections", {{"sections", sections}, {"replace", true}}));
    CHECK(p.sections.size() == sections.size());
    REQUIRE(undo.undo());
    CHECK(p.sections.empty());
    REQUIRE(reg.execute(ctx, "AnalyzeMix", {{"startBeat", 0.0}, {"endBeat", 32.0}}));
    CHECK(ctx.result.contains("master"));
    REQUIRE(reg.execute(ctx, "ProjectCheck", json::object()));
    CHECK(ctx.result["findings"].is_array());
}
