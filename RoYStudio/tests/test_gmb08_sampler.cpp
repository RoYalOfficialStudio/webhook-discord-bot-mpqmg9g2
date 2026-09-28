// GMB 08 - Sampler, sample tools, slicing, root/BPM/key detection, stems.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "dsp/FFT.h"
#include "instruments/Sampler.h"
#include "sampler/SampleTools.h"
#include "stems/StemSeparator.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

double domHz(const std::vector<float>& x, size_t from, int n = 8192) {
    auto mag = dsp::magnitudeSpectrum(x.data() + from, n);
    size_t best = 1;
    for (size_t i = 2; i + 1 < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    const double a = mag[best - 1], b = mag[best], c = mag[best + 1];
    return (static_cast<double>(best) + 0.5 * (a - c) / (a - 2 * b + c)) * SR / n;
}

std::vector<float> play(RoySampler& s, std::vector<NoteEvent> evs, int blocks) {
    AudioBuffer buf(2, 256);
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) {
        buf.clear();
        std::vector<NoteEvent> now;
        for (auto e : evs)
            if (e.offset >= b * 256 && e.offset < (b + 1) * 256) {
                e.offset -= b * 256;
                now.push_back(e);
            }
        auto blk = buf.block();
        s.process(blk, nullptr, now.data(), static_cast<int>(now.size()));
        out.insert(out.end(), buf.channel(0), buf.channel(0) + 256);
    }
    return out;
}
NoteEvent ev(NoteEvent::Type t, int note, int at, float vel = 1.0f) {
    NoteEvent e;
    e.type = t;
    e.note = static_cast<int16_t>(note);
    e.offset = at;
    e.velocity = vel;
    return e;
}
} // namespace

TEST_CASE("gmb08", "sampler key mapping, pitch, velocity layers") {
    auto a440 = makeSine(SR, 440.0, 1.0, 0.5f, 1);
    auto noise = std::make_shared<AudioData>(*makeSine(SR, 3000.0, 1.0, 0.5f, 1));
    RoySampler s;
    SamplerZone soft, hard;
    soft.assetId = "a";
    soft.rootNote = 69;
    soft.highVel = 90;
    hard = soft;
    hard.assetId = "b";
    hard.lowVel = 91;
    hard.highVel = 127;
    s.setZones({soft, hard});
    CHECK(s.requiredAssets().size() == 2);
    s.setAsset("a", a440);
    s.setAsset("b", noise);
    s.prepare(SR, 256);
    auto up = play(s, {ev(NoteEvent::NoteOn, 81, 0, 0.5f)}, 60); // one octave up, soft layer
    CHECK_NEAR(domHz(up, 2000), 880.0, 2.0);
    s.reset();
    auto hardOut = play(s, {ev(NoteEvent::NoteOn, 69, 0, 1.0f)}, 60);
    CHECK_NEAR(domHz(hardOut, 2000), 3000.0, 3.0);
    // note off releases
    s.reset();
    auto rel = play(s, {ev(NoteEvent::NoteOn, 69, 0, 0.5f), ev(NoteEvent::NoteOff, 69, 4800)}, 60);
    CHECK(peak(rel, 4800 + 4800) < 1e-4f);
    CHECK(s.activeVoices() == 0);
    // state roundtrip keeps zones
    auto st = s.saveState();
    RoySampler s2;
    s2.loadState(st);
    CHECK(s2.zones().size() == 2);
    CHECK(s2.zones()[1].lowVel == 91);
}

TEST_CASE("gmb08", "sampler loop, one-shot, reverse, choke") {
    auto ramp = std::make_shared<AudioData>();
    ramp->sampleRate = SR;
    ramp->numChannels = 1;
    ramp->numFrames = 4800;
    ramp->channels = {std::vector<float>(4800)};
    for (int i = 0; i < 4800; ++i) ramp->channels[0][static_cast<size_t>(i)] = static_cast<float>(i) / 4800.0f;
    RoySampler s;
    SamplerZone z;
    z.assetId = "r";
    z.rootNote = 60;
    z.loopMode = 1;
    z.loopStart = 2400;
    z.loopEnd = 4800;
    s.setZones({z});
    s.setAsset("r", ramp);
    s.prepare(SR, 256);
    auto looped = play(s, {ev(NoteEvent::NoteOn, 60, 0)}, 100); // 25600 samples, far longer than the sample
    CHECK(peak(looped, 20000, 25000) > 0.4f);                     // still playing (looping)
    // reverse starts at the end
    z.loopMode = 0;
    z.reverse = true;
    s.setZones({z});
    s.reset();
    auto rev = play(s, {ev(NoteEvent::NoteOn, 60, 0)}, 4);
    CHECK(rev[200] > 0.9f); // starts at the end of the ramp (after the 1 ms attack)
    // one-shot ignores note-off
    z.reverse = false;
    z.oneShot = true;
    s.setZones({z});
    s.reset();
    auto os = play(s, {ev(NoteEvent::NoteOn, 60, 0), ev(NoteEvent::NoteOff, 60, 100)}, 18);
    CHECK(os[3000] > 0.5f);
    // choke group: second pad cuts the first
    SamplerZone a = z, b = z;
    a.lowNote = a.highNote = 36;
    b.lowNote = b.highNote = 37;
    b.rootNote = 37;
    a.chokeGroup = b.chokeGroup = 1;
    s.setZones({a, b});
    s.reset();
    play(s, {ev(NoteEvent::NoteOn, 36, 0), ev(NoteEvent::NoteOn, 37, 10)}, 1);
    CHECK(s.activeVoices() == 1);
}

TEST_CASE("gmb08", "sample editing tools") {
    std::vector<std::vector<float>> in = {std::vector<float>(1000, 0.0f)};
    for (int i = 200; i < 800; ++i) in[0][static_cast<size_t>(i)] = 0.25f;
    int64_t removed = 0;
    auto t = sampler::trimSilence(in, -60.0, &removed);
    CHECK(t[0].size() == 600);
    CHECK(removed == 200);
    auto n = sampler::normalize(t, 0.0);
    CHECK_NEAR(peak(n[0]), 1.0, 1e-6);
    auto r = sampler::reverse({{1.0f, 2.0f, 3.0f}});
    CHECK(r[0] == std::vector<float>({3.0f, 2.0f, 1.0f}));
    auto f = sampler::fade(n, 100, 100);
    CHECK(f[0][0] == 0.0f);
    CHECK(f[0][599] == 0.0f);
    CHECK(in[0][300] == 0.25f); // input untouched
}

TEST_CASE("gmb08", "transient slicing and SLICE TO PADS") {
    std::vector<std::vector<float>> loop = {std::vector<float>(static_cast<size_t>(SR * 2), 0.0f)};
    const std::vector<double> hits = {0.0, 0.25, 0.5, 0.875, 1.25, 1.5};
    for (double h : hits)
        for (int i = 0; i < 3000; ++i)
            loop[0][static_cast<size_t>(h * SR) + static_cast<size_t>(i)] += static_cast<float>(0.7 * std::exp(-i / 500.0) * std::sin(kTwoPi * 200.0 * i / SR));
    auto slices = sampler::slicesFromTransients(loop, SR);
    REQUIRE(slices.size() == hits.size());
    for (size_t i = 0; i < hits.size(); ++i) CHECK(std::llabs(slices[i].first - static_cast<int64_t>(hits[i] * SR)) < 200);
    CHECK(slices.back().second == static_cast<int64_t>(loop[0].size()));
    auto grid = sampler::slicesGrid(1000, 4);
    CHECK(grid[3].first == 750);
    auto zones = sampler::sliceToPads("loop", slices, 36, true);
    REQUIRE(zones.size() == hits.size());
    CHECK(zones[2].lowNote == 38);
    CHECK(zones[2].oneShot);
    CHECK(zones[2].start == slices[2].first);
    // pads play their slice
    RoySampler s;
    s.setZones(zones);
    auto d = std::make_shared<AudioData>();
    d->sampleRate = SR;
    d->numChannels = 1;
    d->channels = loop;
    d->numFrames = static_cast<int64_t>(loop[0].size());
    s.setAsset("loop", d);
    s.prepare(SR, 256);
    auto out = play(s, {ev(NoteEvent::NoteOn, 39, 0)}, 70); // slice 3 (0.875 .. 1.25 s)
    CHECK(peak(out, 0, 300) > 0.5f);
    CHECK(peak(out, static_cast<size_t>(0.38 * SR)) < 1e-6f); // stops at the slice end
}

TEST_CASE("gmb08", "root note, bpm and key detection") {
    // an 808-like sample at A1 (55 Hz) with a pitch drop at the start
    std::vector<std::vector<float>> s808 = {std::vector<float>(static_cast<size_t>(SR * 1.2))};
    double ph = 0;
    for (size_t i = 0; i < s808[0].size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        const double f = 55.0 * (1.0 + 0.8 * std::exp(-t / 0.02));
        ph += f / SR;
        s808[0][i] = static_cast<float>(0.8 * std::sin(kTwoPi * ph) * std::exp(-t / 0.8));
    }
    auto root = sampler::detectRootNote(s808, SR);
    CHECK(root.note == 33);
    CHECK(std::fabs(root.cents) < 15.0);
    // loop at 90 BPM, A minor chord pad
    std::vector<std::vector<float>> loop = {std::vector<float>(static_cast<size_t>(SR * 8), 0.0f)};
    for (double b = 0; b * 60.0 / 90.0 < 7.8; b += 1.0)
        for (int i = 0; i < 2000; ++i)
            loop[0][static_cast<size_t>(b * 60.0 / 90.0 * SR) + static_cast<size_t>(i)] += static_cast<float>(0.5 * std::exp(-i / 300.0) * std::sin(kTwoPi * 120.0 * i / SR));
    for (size_t i = 0; i < loop[0].size(); ++i)
        for (int m : {57, 60, 64, 69, 72, 76, 62, 65})
            loop[0][i] += 0.03f * static_cast<float>(std::sin(kTwoPi * midiToHz(m) * static_cast<double>(i) / SR));
    auto info = sampler::analyzeSample(loop, SR);
    CHECK_NEAR(info.bpm, 90.0, 1.0);
    CHECK(info.keyRoot == 9);
    CHECK(info.keyMinor);
    CHECK(info.looksLikeLoop);
    CHECK(info.transients >= 10);
}

TEST_CASE("gmb08", "stem separation: exact reconstruction, measured quality") {
    stems::registerBuiltinSeparators();
    auto* sep = stems::StemRegistry::instance().find("roy.dsp-basic");
    REQUIRE(sep != nullptr);
    std::string why;
    CHECK(sep->available(&why));
    const size_t n = static_cast<size_t>(SR * 4);
    std::vector<float> drums(n, 0.0f), bass(n, 0.0f), vox(n, 0.0f);
    Rng rng(2);
    for (double t = 0; t < 3.9; t += 0.25)
        for (size_t i = 0; i < 2400; ++i) {
            const size_t k = static_cast<size_t>(t * SR) + i;
            drums[k] += static_cast<float>(rng.uniform(-0.5, 0.5) * std::exp(-static_cast<double>(i) / 300.0));
        }
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SR;
        bass[i] = static_cast<float>(0.4 * std::sin(kTwoPi * 55.0 * t));
        vox[i] = static_cast<float>(0.2 * std::sin(kTwoPi * 440.0 * t) + 0.1 * std::sin(kTwoPi * 880.0 * t));
    }
    std::vector<std::vector<float>> mix = {std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) mix[0][i] = mix[1][i] = drums[i] + bass[i] + vox[i];
    auto res = sep->separate(mix, SR);
    REQUIRE(res.stems.size() == 4);
    CHECK(res.quality.reconstructionErrorDb < -100.0);
    CHECK(!res.quality.warnings.empty());
    const double sBass = stems::sdr({bass, bass}, *res.find("Bass"));
    const double sVox = stems::sdr({vox, vox}, *res.find("Vocals"));
    const double sDrums = stems::sdr({drums, drums}, *res.find("Drums"));
    // Report measured values honestly; require only that each stem is closer to its source than silence (0 dB).
    std::printf("       stem SDR: bass %.1f dB, vocals %.1f dB, drums %.1f dB, crosstalk %.2f\n", sBass, sVox, sDrums, res.quality.crossTalk);
    CHECK(sBass > 3.0);
    CHECK(sVox > 3.0);
    CHECK(sDrums > 0.0);
    auto dir = tempDir("stems");
    auto files = stems::writeStems(res, SR, dir, "song");
    CHECK(files.size() == 4);
    for (auto& f : files) CHECK(std::filesystem::exists(f));
}

#include "commands/Commands.h"
#include "io/AudioFile.h"

TEST_CASE("gmb08", "import, slice-to-pads and stems commands") {
    registerBuiltinProcessors();
    auto dir = tempDir("prod_cmds");
    std::filesystem::create_directories(dir / "Audio");
    // an external loop file (outside the project)
    std::vector<float> loop(static_cast<size_t>(SR * 2), 0.0f);
    for (double h : {0.0, 0.5, 1.0, 1.5})
        for (int i = 0; i < 3000; ++i) loop[static_cast<size_t>(h * SR) + static_cast<size_t>(i)] = static_cast<float>(0.6 * std::exp(-i / 400.0) * std::sin(kTwoPi * 180.0 * i / SR));
    const auto ext = tempDir("prod_cmds_ext") / "loop.wav";
    REQUIRE(writeWavFile(ext, {loop}, SR, SampleFormat::Pcm24));
    const std::string extHash = files::sha256File(ext);

    Project p = makeNewProject("prod", SR, 120.0);
    AudioEngine engine;
    engine.prepare(SR, 256);
    ProjectRuntime rt(engine);
    rt.setProjectDirectory(dir);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, &rt, dir};
    const std::string tid = addTrack(p, TrackType::Audio, "Loop").id;
    REQUIRE(reg.execute(ctx, "ImportAudio", {{"path", ext.string()}, {"trackId", tid}}));
    const std::string assetId = ctx.result["assetId"];
    CHECK(std::filesystem::exists(dir / "Audio" / "loop.wav")); // copied into the project
    CHECK(files::sha256File(ext) == extHash);                  // source untouched
    CHECK(p.findAsset(assetId)->frames == static_cast<int64_t>(loop.size()));
    REQUIRE(reg.execute(ctx, "AnalyzeSample", {{"assetId", assetId}}));
    CHECK(ctx.result["transients"].get<int>() == 4);
    REQUIRE(reg.execute(ctx, "SliceToPads", {{"assetId", assetId}, {"name", "Chops"}}));
    CHECK(ctx.result["slices"] == 4);
    const std::string chops = ctx.result["trackId"];
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, static_cast<int64_t>(SR * 2));
    auto onsets = dsp::detectOnsets(out[0].data(), static_cast<int64_t>(out[0].size()), SR);
    CHECK(onsets.size() >= 4); // audio track + sliced track play the same hits
    REQUIRE(reg.execute(ctx, "SeparateStems", {{"assetId", assetId}}));
    CHECK(ctx.result["stems"].size() == 4);
    CHECK(ctx.result["quality"]["warnings"].size() > 0);
    CHECK(std::filesystem::exists(dir / "Stems"));
    const size_t tracksWithStems = p.tracks.size();
    REQUIRE(undo.undo()); // stems undone from the model (files stay on disk)
    CHECK(p.tracks.size() == tracksWithStems - 4);
    CHECK(p.findTrack(chops) != nullptr);
}
