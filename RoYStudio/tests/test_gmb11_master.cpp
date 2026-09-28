// GMB 11 - Mastering, loudness targets, reference comparison, export (WAV/FLAC), stems.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"
#include "export/Exporter.h"
#include "export/FlacEncoder.h"
#include "io/AudioFile.h"
#include "intelligence/MixIntelligence.h"
#include "master/Mastering.h"

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

std::vector<float> noiseMusic(double seconds, double amp, uint64_t seed) {
    Rng rng(seed);
    std::vector<float> v(static_cast<size_t>(seconds * SR));
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        const double kick = std::exp(-std::fmod(t, 0.5) / 0.08) * std::sin(kTwoPi * 55.0 * std::fmod(t, 0.5));
        v[i] = static_cast<float>(amp * (0.6 * kick + 0.2 * rng.uniform(-1, 1) + 0.3 * std::sin(kTwoPi * 220.0 * t)));
    }
    return v;
}

struct ExportRig {
    AudioEngine engine;
    std::unique_ptr<ProjectRuntime> rt;
    Project p;
    fs::path dir;
    ExportRig(const std::string& name) {
        registerBuiltinProcessors();
        dir = tempDir(name);
        engine.prepare(SR, 512);
        rt = std::make_unique<ProjectRuntime>(engine);
        p = makeNewProject(name, SR, 120.0);
    }
    std::string addAudio(const std::string& trackName, std::shared_ptr<AudioData> d, double startBeat, const std::string& role = {}) {
        const std::string asset = addMemoryAsset(p, *rt, d);
        const std::string tid = addTrack(p, TrackType::Audio, trackName).id;
        p.findTrack(tid)->role = role;
        addClip(*p.findTrack(tid), asset, startBeat, p.tempo.secondsToBeat(static_cast<double>(d->numFrames) / SR));
        return tid;
    }
};
} // namespace

TEST_CASE("gmb11", "FLAC encoder is lossless (decoded by an independent decoder)") {
    auto dir = tempDir("flac");
    Rng rng(3);
    for (int bits : {16, 24})
        for (int channels : {1, 2}) {
            const size_t n = 4096 * 5 + 1234; // not a multiple of the block size
            std::vector<std::vector<int32_t>> s(static_cast<size_t>(channels), std::vector<int32_t>(n));
            const int32_t maxV = (1 << (bits - 1)) - 1;
            for (size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / SR;
                const double v = 0.6 * std::sin(kTwoPi * 440.0 * t) + 0.002 * rng.uniform(-1, 1); // tone + realistic noise floor
                s[0][i] = static_cast<int32_t>(std::lround(v * maxV));
                if (channels == 2) s[1][i] = i < 8192 ? 0 : static_cast<int32_t>(std::lround(0.3 * std::sin(kTwoPi * 330.0 * t) * maxV)); // constant block + tone
            }
            s[0][100] = maxV;
            s[0][101] = -maxV - 1; // extremes
            const fs::path f = dir / std::format("t{}_{}.flac", bits, channels);
            std::string err;
            REQUIRE(flac::encode(f, s, 48000, bits, {}, &err));
            CHECK_MSG(fs::file_size(f) < n * static_cast<size_t>(channels) * static_cast<size_t>(bits / 8), std::format("{} bit {} ch size {} raw {}", bits, channels, fs::file_size(f), n * static_cast<size_t>(channels) * static_cast<size_t>(bits / 8))); // it compresses
            AudioData d;
            REQUIRE(readAudioFile(f, d, &err));
            REQUIRE(d.numChannels == channels);
            REQUIRE(d.numFrames == static_cast<int64_t>(n));
            int64_t mismatches = 0;
            const double scale = bits == 16 ? 32768.0 : 8388608.0;
            for (int c = 0; c < channels; ++c)
                for (size_t i = 0; i < n; ++i)
                    if (std::lround(static_cast<double>(d.channels[static_cast<size_t>(c)][i]) * scale) != s[static_cast<size_t>(c)][i]) ++mismatches;
            CHECK_MSG(mismatches == 0, std::format("{} bit {} ch: {} mismatching samples", bits, channels, mismatches));
        }
}

TEST_CASE("gmb11", "dither: TPDF statistics and noise shaping") {
    std::vector<std::vector<float>> x = {std::vector<float>(96000)};
    for (size_t i = 0; i < x[0].size(); ++i) x[0][i] = static_cast<float>(0.25 * std::sin(kTwoPi * 997.0 * i / SR));
    auto plain = exporting::quantize(x, 16, exporting::Dither::None);
    auto tpdf = exporting::quantize(x, 16, exporting::Dither::Tpdf);
    auto shaped = exporting::quantize(x, 16, exporting::Dither::TpdfShaped);
    auto err = [&](const std::vector<std::vector<int32_t>>& q) {
        std::vector<float> e(q[0].size());
        for (size_t i = 0; i < e.size(); ++i) e[i] = static_cast<float>(q[0][i] - x[0][i] * 32767.0);
        return e;
    };
    auto ep = err(plain), et = err(tpdf), es = err(shaped);
    double mean = 0, var = 0, maxAbs = 0;
    for (float v : et) mean += v;
    mean /= static_cast<double>(et.size());
    for (float v : et) {
        var += (v - mean) * (v - mean);
        maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(v)));
    }
    var /= static_cast<double>(et.size());
    CHECK(std::fabs(mean) < 0.02);
    CHECK_NEAR(var, 0.25, 0.05); // TPDF (1/6) + rounding (1/12) = 0.25 LSB^2
    CHECK(maxAbs <= 1.5 + 1e-6);
    for (float v : ep) CHECK(std::fabs(v) <= 0.5 + 1e-3);
    auto band = [&](const std::vector<float>& e, double lo, double hi) {
        auto m = dsp::magnitudeSpectrum(e.data() + 1000, 32768);
        double s = 0;
        for (size_t k = 0; k < m.size(); ++k) {
            const double f = static_cast<double>(k) * SR / 32768;
            if (f >= lo && f < hi) s += m[k] * m[k];
        }
        return s;
    };
    CHECK(band(es, 100, 4000) < 0.5 * band(et, 100, 4000)); // shaped: less noise where the ear is sensitive
}

TEST_CASE("gmb11", "export mixdown WAV/FLAC, SRC, normalisation, selection") {
    ExportRig r("export_basic");
    r.addAudio("Beat", std::make_shared<AudioData>(AudioData{"", SR, 1, static_cast<int64_t>(SR * 3), {noiseMusic(3.0, 0.3, 1)}}), 0.0);
    exporting::ExportOptions o;
    o.folder = r.dir;
    o.baseName = "song";
    o.bitDepth = 24;
    o.tailSeconds = 0.0;
    auto res = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(res.ok);
    REQUIRE(res.files.size() == 1);
    AudioData back;
    REQUIRE(readAudioFile(res.files[0].path, back));
    CHECK(back.numFrames == static_cast<int64_t>(SR * 3));
    auto direct = render(r.engine, static_cast<int64_t>(SR * 3));
    double md = 0;
    for (size_t i = 0; i < direct[0].size(); i += 7) md = std::max(md, std::fabs(double(back.channels[0][i]) - direct[0][i]));
    CHECK(md < 3.0 / 8388607.0); // 24-bit with TPDF dither
    // never overwrites: a second export gets a new name
    auto res2 = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(res2.ok);
    CHECK(res2.files[0].path != res.files[0].path);
    // FLAC at 44.1 kHz, 16 bit
    o.format = exporting::Format::Flac;
    o.sampleRate = 44100;
    o.bitDepth = 16;
    auto rf = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rf.ok);
    AudioData fl;
    REQUIRE(readAudioFile(rf.files[0].path, fl));
    CHECK(fl.sampleRate == 44100);
    CHECK(std::llabs(fl.numFrames - static_cast<int64_t>(44100 * 3)) <= 1);
    // loudness normalisation to -16 LUFS
    o.format = exporting::Format::Wav;
    o.sampleRate = 0;
    o.bitDepth = 32;
    o.normalize = exporting::Normalize::Loudness;
    o.normalizeLufs = -16.0;
    o.truePeakCeilingDb = -1.0;
    auto rn = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rn.ok);
    AudioData nb;
    REQUIRE(readAudioFile(rn.files[0].path, nb));
    auto st = dsp::measureLoudness(nb.channels, SR);
    CHECK_NEAR(st.integratedLufs, -16.0, 0.2);
    CHECK(st.truePeakDb <= -0.9);
    // peak normalisation
    o.normalize = exporting::Normalize::Peak;
    o.normalizePeakDb = -3.0;
    auto rp = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rp.ok);
    CHECK_NEAR(rp.files[0].stats.samplePeakDb, -3.0, 0.01);
    // selection: beats 2..4 = 1 s
    o.normalize = exporting::Normalize::None;
    o.range = exporting::Range::Selection;
    o.startBeat = 2.0;
    o.endBeat = 4.0;
    auto rs = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rs.ok);
    CHECK_NEAR(rs.renderedSeconds, 1.0, 1e-6);
    // MP3 (LAME) export of the same selection; an invalid bitrate is rejected with a clear error
    o.format = exporting::Format::Mp3;
    o.mp3.bitrateKbps = 192;
    auto rm = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE_MSG_OK(rm.ok, rm.error);
    CHECK(rm.files[0].path.extension() == ".mp3");
    o.mp3.bitrateKbps = 100;
    auto bad = exporting::exportProject(r.engine, *r.rt, r.p, o);
    CHECK(!bad.ok);
    CHECK(bad.error.find("bitrate") != std::string::npos);
}

TEST_CASE("gmb11", "export tails, plugin delay compensation, stems, instrumental") {
    ExportRig r("export_stems");
    auto imp = std::make_shared<AudioData>(AudioData{"", SR, 1, static_cast<int64_t>(SR), {std::vector<float>(static_cast<size_t>(SR), 0.0f)}});
    imp->channels[0][0] = 0.8f; // impulse at 0
    const std::string drums = r.addAudio("Drums", imp, 0.0, "drums");
    const std::string vox = r.addAudio("Vocal", std::make_shared<AudioData>(AudioData{"", SR, 1, static_cast<int64_t>(SR), {noiseMusic(1.0, 0.2, 7)}}), 0.0, "vocal");
    // reverb on the vocal -> automatic tail; limiter on drums -> latency (PDC)
    PluginSlot rev;
    rev.id = files::newId();
    rev.typeId = "roy.reverb";
    rev.state = {{"params", {{"decay", 2.0}, {"mix", 0.3}}}};
    r.p.findChannel(r.p.findTrack(vox)->channelId)->inserts.push_back(rev);
    PluginSlot lim;
    lim.id = files::newId();
    lim.typeId = "roy.limiter";
    r.p.findChannel(r.p.findTrack(drums)->channelId)->inserts.push_back(lim);
    exporting::ExportOptions o;
    o.folder = r.dir;
    o.bitDepth = 32;
    o.stems = exporting::Stems::AllTracks;
    auto res = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(res.ok);
    REQUIRE(res.files.size() == 3);
    CHECK(res.renderedSeconds > 1.0 + 2.0); // automatic reverb tail included
    AudioData mix, d;
    REQUIRE(readAudioFile(res.files[0].path, mix));
    REQUIRE(readAudioFile(res.files[1].path, d));
    CHECK(argmaxAbs(d.channels[0]) == 0);  // PDC: the latent limiter output is aligned to 0
    CHECK(std::fabs(mix.channels[0][0]) > 0.5f);
    // stems sum to the mixdown (unity busses, no master processing)
    AudioData v;
    REQUIRE(readAudioFile(res.files[2].path, v));
    double md = 0;
    for (size_t i = 0; i < mix.channels[0].size(); i += 11) md = std::max(md, std::fabs(double(d.channels[0][i] + v.channels[0][i]) - mix.channels[0][i]));
    CHECK(md < 1e-5);
    // instrumental = mix without vocal tracks; the mute state is restored afterwards
    o.stems = exporting::Stems::Instrumental;
    o.mixdown = false;
    auto ri = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(ri.ok);
    REQUIRE(ri.files.size() == 1);
    AudioData inst;
    REQUIRE(readAudioFile(ri.files[0].path, inst));
    CHECK(rms(inst.channels[0], 2000, 40000) < 1e-6);
    CHECK(!r.p.findChannel(r.p.findTrack(vox)->channelId)->mute);
    // busses and vocal stems
    o.stems = exporting::Stems::MixerBusses;
    auto rb = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rb.ok);
    CHECK(rb.files.size() == 4); // VOCALS, DRUMS, MUSIC, FX
    o.stems = exporting::Stems::VocalStems;
    auto rv = exporting::exportProject(r.engine, *r.rt, r.p, o);
    REQUIRE(rv.ok);
    CHECK(rv.files.size() == 1);
}

TEST_CASE("gmb11", "master assistant reaches targets without extreme limiting") {
    std::vector<std::vector<float>> pre = {noiseMusic(6.0, 0.08, 11), noiseMusic(6.0, 0.08, 12)};
    for (auto& c : pre) // snare-like spikes at -9 dBFS: reaching -14 LUFS needs a few dB of peak limiting
        for (double t = 0.25; t < 6.0; t += 1.0)
            for (size_t i = 0; i < 600; ++i) c[static_cast<size_t>(t * SR) + i] += static_cast<float>(0.35 * std::exp(-static_cast<double>(i) / 150.0) * ((i % 7) < 3 ? 1.0 : -1.0));
    const auto* streaming = master::findPreset("streaming");
    REQUIRE(streaming != nullptr);
    auto res = master::assist(pre, SR, *streaming, 12.0);
    CHECK(res.before.integratedLufs < -20.0);
    CHECK(res.targetReached);
    CHECK_NEAR(res.after.integratedLufs, -14.0, 0.3);
    CHECK(res.after.truePeakDb <= -0.7);
    CHECK(-res.maxGainReductionDb <= 12.0 + 0.1);
    CHECK(-res.maxGainReductionDb > 0.5); // this material really needs limiting
    // with a tiny limiting budget the target is honestly reported as not reached
    auto capped = master::assist(pre, SR, *streaming, 0.5);
    CHECK(!capped.targetReached);
    CHECK(!capped.notes.empty());
    CHECK(master::presets().size() >= 4);
}

TEST_CASE("gmb11", "reference comparison and master chain command") {
    std::vector<float> ref = noiseMusic(6.0, 0.2, 21);
    std::vector<float> mix(ref.size());
    dsp::Biquad shelf;
    shelf.set(dsp::Biquad::Type::HighShelf, SR, 6000.0, 0.707, 6.0);
    for (size_t i = 0; i < ref.size(); ++i) mix[i] = shelf.process(ref[i]) * 0.5f;
    auto c = master::compareToReference({mix, mix}, {ref, ref}, SR);
    CHECK(c.mix.integratedLufs < c.reference.integratedLufs); // -6 dB gain, partly offset by the HF boost
    const auto& centres = mixi::thirdOctaveCentres();
    double at200 = 0, at12k = 0;
    for (size_t k = 0; k < centres.size(); ++k) {
        if (centres[k] == 12500) at12k = c.bandDiffDb[k];
        if (centres[k] == 200) at200 = c.bandDiffDb[k];
    }
    CHECK_NEAR(at12k - at200, 6.0, 1.0); // the tonal difference (shelf) is measured after loudness matching
    CHECK(c.notes.size() >= 3);
    // CreateMasterChain is undoable and keeps existing master inserts
    registerBuiltinProcessors();
    Project p = makeNewProject("master");
    PluginSlot keep;
    keep.id = "existing";
    keep.typeId = "roy.eq";
    p.master()->inserts.push_back(keep);
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo};
    REQUIRE(reg.execute(ctx, "CreateMasterChain", {{"preset", "club"}}));
    CHECK(p.master()->inserts.size() == 1 + master::findPreset("club")->chain.size());
    CHECK(p.master()->inserts[0].id == "existing");
    REQUIRE(undo.undo());
    CHECK(p.master()->inserts.size() == 1);
}
