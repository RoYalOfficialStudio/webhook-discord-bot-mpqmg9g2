// LIVE VOCAL (hear yourself with autotune while recording) and CHANNEL PRESETS (save a mixed
// vocal chain, load it in the next song).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "core/Files.h"
#include "dsp/FFT.h"
#include "mixer/ChannelPreset.h"

#include <cmath>
#include <format>
#include <numbers>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

struct Live {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("live", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    std::string track, channel;
    Live() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        p.key = Key{0, ScaleType::Major}; // C major
        Track& t = addTrack(p, TrackType::Audio, "Vocal");
        track = t.id;
        channel = t.channelId;
        REQUIRE(rt.rebuild(p));
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s: %s\n", id.c_str(), ctx->error.c_str());
        if (ok) rt.rebuild(p);
        return ok;
    }
    // Feeds a mono "microphone" through the device input with the transport STOPPED and
    // returns the left output.
    std::vector<float> monitor(const std::vector<float>& mic) {
        std::vector<float> out;
        std::vector<float> l(256), r(256);
        for (size_t pos = 0; pos + 256 <= mic.size(); pos += 256) {
            const float* in[1] = {mic.data() + pos};
            float* o[2] = {l.data(), r.data()};
            engine.process(in, 1, o, 2, 256);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
};

std::vector<float> sineMono(double hz, double seconds, float amp) {
    std::vector<float> v(static_cast<size_t>(SR * seconds));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / SR));
    return v;
}

double dominantMidi(const std::vector<float>& y, size_t from) {
    REQUIRE(y.size() >= from + 16384);
    auto mag = dsp::magnitudeSpectrum(y.data() + from, 16384);
    size_t best = 1;
    for (size_t i = 2; i < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    return hzToMidi(static_cast<double>(best) * SR / 16384);
}
} // namespace

TEST_CASE("livevocal", "LIVE VOCAL: the voice is heard in real time, tuned to the song key, with the transport stopped") {
    Live L;
    const auto mic = sineMono(midiToHz(64.4), 1.5, 0.4); // E + 40 cents (sharp)
    // without monitoring nothing of the microphone reaches the speakers
    CHECK(peak(L.monitor(mic)) < 1e-6f);
    const size_t undo0 = L.undo.undoCount();
    REQUIRE(L.run("SetupLiveVocal", {{"trackId", L.track}}));
    CHECK(L.undo.undoCount() == undo0 + 1); // one button = one undo step
    const Track& t = *L.p.findTrack(L.track);
    CHECK(t.monitor);
    CHECK(t.armed);
    const MixerChannel& ch = *L.p.findChannel(L.channel);
    REQUIRE(ch.inserts.size() == 1);
    CHECK(ch.inserts[0].typeId == "roy.vocaltune");
    CHECK(ch.inserts[0].state["params"]["keyRoot"].get<int>() == 0);
    CHECK(ch.inserts[0].state["params"]["keyScale"].get<int>() == static_cast<int>(ScaleType::Major));
    const auto y = L.monitor(mic);
    CHECK(peak(y) > 0.1f);
    CHECK_NEAR(dominantMidi(y, 40000), 64.0, 0.1); // heard in tune
    // tune bypassed: the raw (sharp) voice is heard
    REQUIRE(L.run("BypassInsert", {{"slotId", ch.inserts[0].id}, {"bypass", true}}));
    CHECK_NEAR(dominantMidi(L.monitor(mic), 40000), 64.4, 0.1);
    // pressing LIVE again: no second tune, bypass lifted, key follows a changed song key
    REQUIRE(L.run("SetKey", {{"key", "D Minor"}}));
    REQUIRE(L.run("SetupLiveVocal", {{"trackId", L.track}}));
    const MixerChannel& ch2 = *L.p.findChannel(L.channel);
    CHECK(ch2.inserts.size() == 1);
    CHECK(!ch2.inserts[0].bypass);
    CHECK(ch2.inserts[0].state["params"]["keyRoot"].get<int>() == 2);
    // undo all the way: no tune, no monitoring
    while (L.undo.undoCount() > undo0) L.undo.undo();
    CHECK(L.p.findChannel(L.channel)->inserts.empty());
    CHECK(!L.p.findTrack(L.track)->monitor);
    // instrument tracks are refused
    Track& m = addTrack(L.p, TrackType::Midi, "Keys");
    CHECK(!L.run("SetupLiveVocal", {{"trackId", m.id}}));
}

TEST_CASE("livevocal", "channel preset: save a mixed vocal, load it into the next song (new key), undo, backups") {
    const fs::path dir = fs::temp_directory_path() / ("roy_presets_" + files::newId());
    Live a;
    REQUIRE(a.run("SetupLiveVocal", {{"trackId", a.track}}));
    REQUIRE(a.run("AddInsert", {{"channelId", a.channel}, {"typeId", "roy.compressor"}, {"params", {{"threshold", -27.0}, {"ratio", 6.0}}}}));
    REQUIRE(a.run("SetChannelGain", {{"channelId", a.channel}, {"gainDb", -4.5}}));
    a.rt.captureProcessorStates(a.p);
    const json preset = presets::makeChannelPreset(*a.p.findChannel(a.channel), "My Rap Vocal");
    std::string err;
    REQUIRE(presets::validChannelPreset(preset, &err));
    fs::path file;
    REQUIRE(presets::saveChannelPreset(dir, preset, false, &file, &err));
    CHECK(file.filename() == "My Rap Vocal.roychain");
    CHECK(!presets::saveChannelPreset(dir, preset, false, nullptr, &err)); // never silently replaced
    REQUIRE(presets::saveChannelPreset(dir, preset, true, nullptr, &err));  // replace: old copy kept
    CHECK(fs::exists(dir / "Backups") && !fs::is_empty(dir / "Backups"));
    const auto list = presets::listChannelPresets(dir);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "My Rap Vocal");

    // next song: other key, a vocal track with an old EQ on it
    Live b;
    b.p.key = Key{9, ScaleType::NaturalMinor}; // A minor
    REQUIRE(b.run("AddInsert", {{"channelId", b.channel}, {"typeId", "roy.eq"}}));
    auto loaded = presets::loadChannelPreset(list[0].file, &err);
    REQUIRE(loaded.has_value());
    const size_t u = b.undo.undoCount();
    REQUIRE(b.run("ApplyChannelPreset", {{"channelId", b.channel}, {"preset", *loaded}}));
    CHECK(b.undo.undoCount() == u + 1);
    const MixerChannel& ch = *b.p.findChannel(b.channel);
    REQUIRE(ch.inserts.size() == 2);
    CHECK(ch.inserts[0].typeId == "roy.vocaltune");
    CHECK(ch.inserts[0].state["params"]["keyRoot"].get<int>() == 9); // follows the NEW song
    CHECK(ch.inserts[0].state["params"]["keyScale"].get<int>() == static_cast<int>(ScaleType::NaturalMinor));
    CHECK(ch.inserts[1].typeId == "roy.compressor");
    CHECK_NEAR(ch.inserts[1].state["params"]["threshold"].get<double>(), -27.0, 1e-6);
    CHECK_NEAR(ch.gainDb, -4.5, 1e-6);
    CHECK(ch.inserts[0].id != a.p.findChannel(a.channel)->inserts[0].id); // fresh slots
    auto proc = b.rt.processorForSlot(ch.inserts[1].id);
    REQUIRE(proc != nullptr);
    CHECK_NEAR(proc->getParam(0), -27.0, 1e-4); // the running compressor has the saved settings
    REQUIRE(b.undo.undo());
    REQUIRE(b.p.findChannel(b.channel)->inserts.size() == 1);
    CHECK(b.p.findChannel(b.channel)->inserts[0].typeId == "roy.eq");

    // damaged / foreign files are refused with a reason, nothing changes
    REQUIRE(files::atomicWrite(dir / "broken.roychain", "{not json"));
    CHECK(!presets::loadChannelPreset(dir / "broken.roychain", &err));
    CHECK(err.find("damaged") != std::string::npos);
    CHECK(presets::listChannelPresets(dir).size() == 1);
    CHECK(!b.run("ApplyChannelPreset", {{"channelId", b.channel}, {"preset", {{"format", "other"}}}}));
    // unknown effect types are skipped and reported
    json foreign = *loaded;
    foreign["inserts"].push_back({{"typeId", "roy.doesnotexist"}, {"name", "Missing FX"}});
    REQUIRE(b.run("ApplyChannelPreset", {{"channelId", b.channel}, {"preset", foreign}}));
    CHECK(b.ctx->result["skipped"].size() == 1);
    // file names are safe on Windows
    CHECK(presets::presetFileStem("a/b:c*?") == "a_b_c__");
    CHECK(presets::presetFileStem("CON") == "_CON");
    CHECK(presets::presetFileStem("  ") == "Preset");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("livevocal", "factory vocal chains are valid, use only built-in effects and load") {
    Live L;
    const auto factory = presets::factoryChannelPresets();
    REQUIRE(factory.size() >= 3);
    for (auto& f : factory) {
        std::string err;
        CHECK(presets::validChannelPreset(f, &err));
        for (auto& i : f["inserts"]) CHECK(ProcessorFactory::instance().has(i["typeId"].get<std::string>()));
        REQUIRE(L.run("ApplyChannelPreset", {{"channelId", L.channel}, {"preset", f}}));
        CHECK(L.ctx->result["skipped"].empty());
        CHECK(L.p.findChannel(L.channel)->gainDb == 0.0f); // factory chains leave the fader alone
    }
}

TEST_CASE("livevocal", "shipped vocal chains (UserData presets of the portable build) load and sound sane live") {
#ifdef ROY_SHIPPED_PRESETS_DIR
    const fs::path dir = ROY_SHIPPED_PRESETS_DIR;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return; // test kit copied to another machine
    const auto list = presets::listChannelPresets(dir);
    REQUIRE(!list.empty());
    for (auto& pf : list) {
        Live L;
        std::string err;
        auto pr = presets::loadChannelPreset(pf.file, &err);
        REQUIRE_MSG_OK(pr.has_value(), err);
        REQUIRE(L.run("ApplyChannelPreset", {{"channelId", L.channel}, {"preset", *pr}}));
        CHECK_MSG(L.ctx->result["skipped"].empty(), pf.name);
        CHECK(L.p.findChannel(L.channel)->inserts.size() == (*pr)["inserts"].size());
        REQUIRE(L.run("SetupLiveVocal", {{"trackId", L.track}})); // LIVE keeps the chain, no second tune
        CHECK(L.p.findChannel(L.channel)->inserts.size() == (*pr)["inserts"].size());
        // a loud "voice" (200 Hz + harmonics, sharp) through the whole chain while monitoring
        std::vector<float> mic(static_cast<size_t>(SR * 2.0));
        for (size_t i = 0; i < mic.size(); ++i) {
            const double t = static_cast<double>(i) / SR, f = midiToHz(55.4);
            mic[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * f * t) + 0.25 * std::sin(4 * std::numbers::pi * f * t) +
                                        0.12 * std::sin(6 * std::numbers::pi * f * t));
        }
        const auto y = L.monitor(mic);
        bool finite = true;
        for (float v : y) finite &= std::isfinite(v);
        CHECK_MSG(finite, pf.name);
        const float pk = peak(y, 4800);
        std::printf("    %s: live peak %.3f\n", pf.name.c_str(), pk);
        CHECK_MSG(pk > 0.05f && pk < 0.95f, std::format("{}: peak {:.3f}", pf.name, pk)); // audible, never clipping (limiter)
    }
#endif
}
