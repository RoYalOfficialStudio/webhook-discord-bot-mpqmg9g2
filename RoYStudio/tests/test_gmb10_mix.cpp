// GMB 10 - Mix Intelligence, Dynamic Space in the mix, What-If, Producer Memory.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "dsp/Filters.h"
#include "intelligence/MixIntelligence.h"
#include "intelligence/ProducerMemory.h"
#include "intelligence/WhatIf.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

std::vector<float> bandNoise(double lo, double hi, double seconds, double amp, uint64_t seed, double gateOn = 1.0, double gatePeriod = 0.0) {
    Rng rng(seed);
    dsp::Biquad hp, lp, hp2, lp2;
    hp.set(dsp::Biquad::Type::HighPass, SR, lo, 0.707);
    hp2.set(dsp::Biquad::Type::HighPass, SR, lo, 0.707);
    lp.set(dsp::Biquad::Type::LowPass, SR, hi, 0.707);
    lp2.set(dsp::Biquad::Type::LowPass, SR, hi, 0.707);
    std::vector<float> v(static_cast<size_t>(seconds * SR));
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        const bool on = gatePeriod <= 0 || std::fmod(t, gatePeriod) < gateOn;
        v[i] = on ? static_cast<float>(amp) * lp2.process(lp.process(hp2.process(hp.process(static_cast<float>(rng.uniform(-1, 1)))))) : 0.0f;
    }
    return v;
}

bool has(const mixi::MixReport& r, const std::string& type, const std::string& titlePart = {}) {
    for (auto& i : r.issues)
        if (i.type == type && (titlePart.empty() || i.title.find(titlePart) != std::string::npos)) return true;
    return false;
}
} // namespace

TEST_CASE("gmb10", "mix analysis finds masking, collisions, phase, headroom, stereo") {
    const double secs = 4.0;
    auto vocal = bandNoise(1800, 3500, secs, 3.0, 1, 0.5, 0.8);
    auto piano = bandNoise(2000, 3200, secs, 4.0, 2);
    std::vector<float> kick(static_cast<size_t>(secs * SR)), bass(kick.size()), bassInv(kick.size());
    for (size_t i = 0; i < kick.size(); ++i) {
        const double t = static_cast<double>(i) / SR, tb = std::fmod(t, 0.5);
        kick[i] = static_cast<float>(0.8 * std::exp(-tb / 0.12) * std::sin(kTwoPi * 55.0 * tb));
        bass[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 55.0 * t));
        bassInv[i] = -bass[i];
    }
    std::vector<mixi::TrackAudio> tracks = {{"v", "cv", "Vocal", "vocal", {vocal, vocal}},
                                            {"p", "cp", "Piano", "keys", {piano, piano}},
                                            {"k", "ck", "Kick", "drums", {kick, kick}},
                                            {"b", "cb", "808", "808", {bass, bass}}};
    std::vector<float> mL(kick.size()), mR(kick.size());
    for (size_t i = 0; i < mL.size(); ++i) {
        mL[i] = vocal[i] + piano[i] + kick[i] + bass[i];
        mR[i] = vocal[i] + piano[i] + kick[i] - bass[i]; // wide (out of phase) low end on the master
    }
    auto rep = mixi::analyzeMix(tracks, {mL, mR});
    CHECK(has(rep, "masking", "VOCAL <-> PIANO MASKING"));
    for (auto& i : rep.issues)
        if (i.type == "masking" && i.tracks[0] == "Vocal") {
            CHECK(i.lowHz < 2600 && i.highHz > 2600);
            REQUIRE(!i.suggestions.empty());
            CHECK(i.suggestions[0].args["typeId"] == "roy.dynamicspace");
            CHECK(i.suggestions[0].args["sidechain"] == "cv");
        }
    CHECK(has(rep, "low_end_collision"));
    CHECK(has(rep, "stereo", "WIDE LOW END"));
    CHECK(rep.masterPeakDb > 0.0);
    CHECK(has(rep, "clipping", "MASTER"));
    CHECK(rep.toJson()["issues"].size() == rep.issues.size());
    // phase between tracks
    std::vector<mixi::TrackAudio> ph = {{"a", "ca", "Kick A", "drums", {bass, bass}}, {"b", "cb", "Kick B", "drums", {bassInv, bassInv}}};
    auto pr = mixi::analyzeMix(ph, {});
    CHECK(has(pr, "phase"));
    // a clean, separated mix has no masking between vocal and a low pad
    auto pad = bandNoise(100, 400, secs, 1.0, 5);
    std::vector<mixi::TrackAudio> clean = {{"v", "cv", "Vocal", "vocal", {vocal, vocal}}, {"p", "cp", "Pad", "keys", {pad, pad}}};
    auto cr = mixi::analyzeMix(clean, {});
    CHECK(!has(cr, "masking"));
}

TEST_CASE("gmb10", "engine capture and suggested Dynamic Space fix work in the mix") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("mix", SR, 120.0);
    auto vocal = std::make_shared<AudioData>();
    auto vv = bandNoise(2000, 3000, 3.0, 3.0, 3, 0.6, 1.2);
    vocal->sampleRate = SR;
    vocal->numChannels = 1;
    vocal->channels = {vv};
    vocal->numFrames = static_cast<int64_t>(vv.size());
    auto pianoData = std::make_shared<AudioData>();
    auto pv = bandNoise(2000, 3000, 3.0, 3.0, 4);
    pianoData->sampleRate = SR;
    pianoData->numChannels = 1;
    pianoData->channels = {pv};
    pianoData->numFrames = static_cast<int64_t>(pv.size());
    const std::string va = addMemoryAsset(p, rt, vocal), pa = addMemoryAsset(p, rt, pianoData);
    const std::string vt = addTrack(p, TrackType::Audio, "Vocal").id;
    p.findTrack(vt)->role = "vocal";
    const std::string pt = addTrack(p, TrackType::Audio, "Piano").id;
    addClip(*p.findTrack(vt), va, 0, 6);
    addClip(*p.findTrack(pt), pa, 0, 6);
    std::vector<mixi::TrackAudio> tracks;
    mixi::Channels master;
    REQUIRE(mixi::renderForAnalysis(engine, rt, p, 0, 6, tracks, master));
    REQUIRE(tracks.size() == 2);
    // captured tracks sum to the master
    double md = 0;
    for (size_t i = 0; i < master[0].size(); i += 13) md = std::max(md, std::fabs(double(tracks[0].audio[0][i] + tracks[1].audio[0][i]) - master[0][i]));
    CHECK(md < 1e-5);
    auto rep = mixi::analyzeMix(tracks, master);
    const mixi::MixSuggestion* ds = nullptr;
    for (auto& i : rep.issues)
        if (i.type == "masking" && i.tracks[0] == "Vocal")
            for (auto& s : i.suggestions)
                if (s.command == "AddInsert" && s.args["typeId"] == "roy.dynamicspace") ds = &s;
    REQUIRE(ds != nullptr);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt};
    REQUIRE(reg.execute(ctx, ds->command, ds->args));
    REQUIRE(mixi::renderForAnalysis(engine, rt, p, 0, 6, tracks, master));
    // piano output is reduced while the vocal sings (0.1-0.5 s) and not in the pause (0.7-1.1 s)
    const double duckedSinging = rms(tracks[1].audio[0], 4800, 24000) / rms(pv, 4800, 24000);
    const double duckedPause = rms(tracks[1].audio[0], 38400, 52800) / rms(pv, 38400, 52800);
    CHECK_MSG(duckedSinging < 0.75, std::format("while singing {}", duckedSinging));
    CHECK_MSG(duckedPause > 0.85, std::format("in pause {}", duckedPause));
    REQUIRE(undo.undo()); // A/B: the fix is fully reversible
    CHECK(p.findChannel(p.findTrack(pt)->channelId)->inserts.empty());
}

TEST_CASE("gmb10", "what-if A/B, commit as one undo step, discard") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("whatif", SR, 120.0);
    const std::string asset = addMemoryAsset(p, rt, makeSine(SR, 440.0, 2.0, 0.2f));
    const std::string vt = addTrack(p, TrackType::Audio, "Vocal").id;
    addClip(*p.findTrack(vt), asset, 0, 4);
    const std::string ch = p.findTrack(vt)->channelId;
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    int rebuilds = 0;
    CommandContext ctx{p, undo, &rt};
    ctx.changed = [&](bool) { ++rebuilds; };
    whatif::Steps steps = {{"SetChannelGain", {{"channelId", ch}, {"gainDb", 6.0}, {"relative", true}}},
                           {"SetChannelWidth", {{"channelId", ch}, {"width", 1.6}}},
                           {"AddInsert", {{"channelId", ch}, {"typeId", "roy.vocaltune"}, {"params", {{"strength", 0.6}}}}}};
    whatif::WhatIfSession s(reg, ctx);
    REQUIRE(s.begin("Vocal +6 dB, wider, VocalTune 60%", steps));
    CHECK(p.findChannel(ch)->gainDb == 6.0f);
    s.showA();
    CHECK(p.findChannel(ch)->gainDb == 0.0f);
    CHECK(p.findChannel(ch)->inserts.empty());
    s.showB();
    CHECK(p.findChannel(ch)->width == 1.6f);
    CHECK(rebuilds >= 3);
    s.discard();
    CHECK(p.findChannel(ch)->gainDb == 0.0f);
    CHECK(!undo.canUndo());
    REQUIRE(s.begin("again", steps));
    REQUIRE(s.commit());
    CHECK(undo.undoCount() == 1);
    CHECK(p.findChannel(ch)->gainDb == 6.0f);
    REQUIRE(undo.undo());
    CHECK(p.findChannel(ch)->gainDb == 0.0f);
    // a failing proposal leaves the project untouched
    CHECK(!s.begin("bad", {{"SetChannelGain", {{"channelId", "nope"}, {"gainDb", 1.0}}}}));
    CHECK(!s.active());
    // offline comparison
    auto cmp = whatif::compareOffline(engine, reg, ctx, {{"SetChannelGain", {{"channelId", ch}, {"gainDb", 6.0}}}}, 0, 4);
    REQUIRE(cmp.has_value());
    CHECK_NEAR(cmp->lufsB - cmp->lufsA, 6.0, 0.2);
    CHECK(p.findChannel(ch)->gainDb == 0.0f); // unchanged after the comparison
}

TEST_CASE("gmb10", "producer memory: learn, hints, reset, global file") {
    Project p = makeNewProject("mem");
    const std::string v = addTrack(p, TrackType::Audio, "Lead Vocal").id;
    p.findTrack(v)->role = "vocal";
    const std::string m = addTrack(p, TrackType::Audio, "Keys").id;
    const std::string a = addTrack(p, TrackType::Audio, "Adlib L").id;
    p.findTrack(a)->role = "adlib";
    p.findChannel(p.findTrack(v)->channelId)->gainDb = 2.0f;
    p.findChannel(p.findTrack(m)->channelId)->gainDb = -4.0f;
    p.findChannel(p.findTrack(a)->channelId)->pan = -0.7f;
    memory::ProducerMemory mem;
    mem.learnFromProject(p);
    REQUIRE(mem.get("vocal_vs_music_db").has_value());
    CHECK(mem.get("vocal_vs_music_db")->value > 3.0);
    CHECK(mem.get("adlib_width")->value > 0.5);
    CHECK(mem.hints(p).empty()); // project matches its own preferences
    p.findChannel(p.findTrack(v)->channelId)->gainDb = -6.0f;
    auto h = mem.hints(p);
    REQUIRE(h.size() == 1);
    CHECK(h[0].key == "vocal_vs_music_db");
    CHECK(h[0].text.find("preference, not a rule") != std::string::npos);
    mem.storeToProject(p);
    memory::ProducerMemory loaded;
    loaded.loadFromProject(p);
    CHECK(loaded.all().size() == mem.all().size());
    auto dir = tempDir("memory");
    REQUIRE(mem.saveGlobal(dir / "producer.json"));
    CHECK(mem.reset("vocal_vs_music_db"));
    CHECK(!mem.get("vocal_vs_music_db"));
    mem.resetAll();
    CHECK(mem.all().empty());
    REQUIRE(mem.loadGlobal(dir / "producer.json"));
    CHECK(mem.get("vocal_vs_music_db")->source == "learned");
}
