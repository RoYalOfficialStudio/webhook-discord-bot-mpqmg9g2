// Browser: audio preview (outside project routing, never exported), tempo sync,
// BPM from file names, drop targets (drum pad sample, sampler).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "browser/SamplePreview.h"
#include "commands/Commands.h"
#include "dsp/FFT.h"
#include "io/AudioFile.h"

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

fs::path writeTone(const fs::path& dir, const std::string& name, double hz, double seconds, float amp = 0.5f) {
    std::vector<float> x(static_cast<size_t>(seconds * SR));
    for (size_t i = 0; i < x.size(); ++i) x[i] = amp * static_cast<float>(std::sin(kTwoPi * hz * static_cast<double>(i) / SR));
    const fs::path p = dir / name;
    std::string err;
    REQUIRE(writeWavFile(p, {x, x}, SR, SampleFormat::Float32, true, &err));
    return p;
}

std::vector<std::vector<float>> pump(AudioEngine& e, int blocks, int block = 256) {
    std::vector<std::vector<float>> out(2);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    float* o[2] = {l.data(), r.data()};
    for (int b = 0; b < blocks; ++b) {
        e.process(nullptr, 0, o, 2, block);
        out[0].insert(out[0].end(), l.begin(), l.end());
        out[1].insert(out[1].end(), r.begin(), r.end());
    }
    return out;
}

double peakHz(const std::vector<float>& x, size_t from) {
    std::vector<float> seg(16384, 0.0f);
    for (size_t i = 0; i < seg.size() && from + i < x.size(); ++i) seg[i] = x[from + i];
    auto mag = dsp::magnitudeSpectrum(seg.data(), 16384);
    size_t pk = 1;
    for (size_t i = 2; i < mag.size(); ++i)
        if (mag[i] > mag[pk]) pk = i;
    return pk * SR / 16384.0;
}
} // namespace

TEST_CASE("browser", "bpm from file names and loop detection") {
    CHECK(browser::bpmFromFileName("Kick_128bpm") == 128);
    CHECK(browser::bpmFromFileName("dark loop 90 BPM") == 90);
    CHECK(browser::bpmFromFileName("trap_140_Am") == 140);
    CHECK(browser::bpmFromFileName("BPM95 guitar") == 95);
    CHECK(browser::bpmFromFileName("snare_03") == 0);
    CHECK(browser::bpmFromFileName("808_long") == 0); // 808 is not a tempo
    CHECK(browser::looksLikeLoop("drum loop", 1.0, 0));
    CHECK(browser::looksLikeLoop("groove", 8.0, 120.0));   // 4 bars
    CHECK(!browser::looksLikeLoop("snare", 0.3, 0));
}

TEST_CASE("browser", "preview is audible, stops, bypasses project routing and exports") {
    registerBuiltinProcessors();
    const fs::path dir = tempDir("preview");
    const fs::path tone = writeTone(dir, "tone.wav", 1000.0, 2.0);
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("prev", SR, 120.0);
    REQUIRE(rt.rebuild(p));
    browser::Previewer pv(engine);
    browser::PreviewOptions o;
    o.gain = 0.5f;
    auto info = pv.preview(tone, o);
    REQUIRE_MSG_OK(info.ok, info.error);
    CHECK(!info.isLoop);
    CHECK(pv.playing());
    auto out = pump(engine, 40);
    CHECK_NEAR(peak(out[0], 2000, 10000), 0.25, 0.01); // 0.5 amplitude x 0.5 preview gain
    CHECK_NEAR(peakHz(out[0], 0), 1000.0, 3.0);
    // project master meter does not see the preview (it is mixed after the master)
    auto mp = rt.channelParams(p.master()->id);
    REQUIRE(mp != nullptr);
    CHECK(mp->peakL.load() < 1e-6f);
    // offline renders never contain the preview, even while it plays
    auto rendered = render(engine, 9600);
    CHECK(peak(rendered[0]) < 1e-6f);
    CHECK(pv.playing());
    // no allocation on the audio thread while previewing
    {
        std::vector<float> l(256), r(256);
        float* oo[2] = {l.data(), r.data()};
        AllocationCounter c;
        for (int i = 0; i < 50; ++i) engine.process(nullptr, 0, oo, 2, 256);
        CHECK(c.count() == 0);
    }
    pv.stop();
    auto after = pump(engine, 20);
    CHECK(peak(after[0]) < 1e-6f);
    // plays once and ends by itself
    pv.preview(tone, o);
    pump(engine, static_cast<int>(2.2 * SR / 256));
    CHECK(!pv.playing());
    pv.collect();
}

TEST_CASE("browser", "tempo sync stretches loops to the project tempo without changing pitch") {
    const fs::path dir = tempDir("previewsync");
    const fs::path loop = writeTone(dir, "loop_100bpm.wav", 440.0, 9.6); // 4 bars @ 100 BPM
    AudioEngine engine;
    engine.prepare(SR, 256);
    browser::Previewer pv(engine);
    browser::PreviewOptions o;
    o.tempo = browser::TempoMode::Project;
    o.projectBpm = 120.0;
    auto info = pv.preview(loop, o);
    REQUIRE(info.ok);
    CHECK(info.sourceBpm == 100.0);
    CHECK(info.bpmSource == "filename");
    CHECK(info.isLoop);
    CHECK_NEAR(info.stretch, 100.0 / 120.0, 1e-9);
    auto out = pump(engine, 100);
    CHECK_NEAR(peakHz(out[0], 4000), 440.0, 4.0); // pitch preserved
    o.tempo = browser::TempoMode::Original;
    auto orig = pv.preview(loop, o);
    CHECK(orig.stretch == 1.0);
    pv.stop();
}

TEST_CASE("browser", "drop targets: drum pad sample and sampler") {
    registerBuiltinProcessors();
    const fs::path dir = tempDir("droptargets");
    const fs::path tone = writeTone(dir, "pad.wav", 1000.0, 0.3, 0.8f);
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    rt.setProjectDirectory(dir);
    Project p = makeNewProject("drop", SR, 120.0);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt, dir};
    // Browser -> Drum Pad
    REQUIRE(reg.execute(ctx, "ImportAudio", {{"path", tone.string()}, {"copy", false}}));
    const std::string asset = ctx.result["assetId"];
    REQUIRE(reg.execute(ctx, "AddTrack", {{"type", "beat"}, {"name", "Drums"}}));
    const std::string beat = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "AddPattern", {{"name", "P"}}));
    const std::string pat = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "SetRowPattern", {{"patternId", pat}, {"voice", "kick"}, {"text", "x..............."}}));
    REQUIRE(reg.execute(ctx, "SetRowSample", {{"patternId", pat}, {"voice", "kick"}, {"assetId", asset}, {"name", "Pad"}}));
    REQUIRE(reg.execute(ctx, "AddPatternClip", {{"trackId", beat}, {"patternId", pat}, {"lengthBeats", 4.0}}));
    REQUIRE(rt.rebuild(p));
    for (auto& w : rt.lastWarnings()) std::printf("       warning: %s\n", w.c_str());
    auto out = render(engine, 12000);
    CHECK_NEAR(peakHz(out[0], 256), 1000.0, 5.0); // the pad plays the dropped sample
    REQUIRE(undo.undo()); // pattern clip
    REQUIRE(undo.undo()); // row sample
    CHECK(p.findPattern(pat)->rows[0].sampleAssetId.empty());
    // Browser -> Sampler
    REQUIRE(reg.execute(ctx, "AddTrack", {{"type", "midi"}, {"name", "Keys"}}));
    const std::string keys = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "LoadSampleIntoSampler", {{"trackId", keys}, {"assetId", asset}, {"rootNote", 60}}));
    CHECK(p.findTrack(keys)->instrument->typeId == "roy.sampler");
    REQUIRE(reg.execute(ctx, "AddMidiClip", {{"trackId", keys}, {"lengthBeats", 4.0}}));
    REQUIRE(reg.execute(ctx, "AddNote", {{"clipId", ctx.result["id"]}, {"pitch", 72}, {"lengthBeats", 1.0}, {"wrongNoteMode", "off"}}));
    REQUIRE(rt.rebuild(p));
    auto s = render(engine, 12000);
    CHECK_NEAR(peakHz(s[0], 256), 2000.0, 10.0); // one octave above the root
}
