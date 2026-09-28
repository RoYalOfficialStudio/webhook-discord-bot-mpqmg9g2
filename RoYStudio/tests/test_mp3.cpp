// MP3 export (LAME 3.100 loaded at runtime): real encode + decode validation for
// CBR bitrates, VBR, mono/stereo, metadata (ID3v2 + ID3v1), long files, error cases.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "core/Process.h"
#include "dsp/FFT.h"
#include "export/Exporter.h"
#include "export/Mp3Encoder.h"
#include "io/AudioFile.h"

#include <chrono>
#include <fstream>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr int SR = 48000;

std::vector<std::vector<float>> music(double seconds, int channels = 2) {
    const size_t n = static_cast<size_t>(seconds * SR);
    std::vector<std::vector<float>> x(static_cast<size_t>(channels), std::vector<float>(n));
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SR;
        const double env = 0.6 + 0.4 * std::sin(kTwoPi * 0.5 * t);
        const double l = 0.25 * std::sin(kTwoPi * 220.0 * t) + 0.15 * std::sin(kTwoPi * 330.0 * t) + 0.08 * std::sin(kTwoPi * 1760.0 * t);
        const double r = 0.25 * std::sin(kTwoPi * 277.18 * t) + 0.15 * std::sin(kTwoPi * 440.0 * t);
        x[0][i] = static_cast<float>(env * l);
        if (channels > 1) x[1][i] = static_cast<float>(env * r);
    }
    return x;
}

// Residual alignment (should be 0: gapless trim) and SNR in dB of `dec` against `ref`.
std::pair<int, double> alignedSnr(const std::vector<float>& ref, const std::vector<float>& dec) {
    int best = 0;
    double bestC = -1e30;
    for (int d = 0; d < 64; ++d) {
        double c = 0;
        for (size_t i = 10000; i < 30000 && i + static_cast<size_t>(d) < dec.size(); i += 3) c += ref[i] * dec[i + static_cast<size_t>(d)];
        if (c > bestC) {
            bestC = c;
            best = d;
        }
    }
    double s = 0, e = 0;
    for (size_t i = 5000; i + 5000 < ref.size() && i + static_cast<size_t>(best) < dec.size(); ++i) {
        const double err = dec[i + static_cast<size_t>(best)] - ref[i];
        s += static_cast<double>(ref[i]) * ref[i];
        e += err * err;
    }
    return {best, 10.0 * std::log10(s / std::max(1e-20, e))};
}

std::string fileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("mp3", "encoder library is available (LAME 3.100)") {
    std::string why;
    REQUIRE_MSG_OK(mp3::available(&why), why);
    CHECK(mp3::encoderVersion().find("3.100") != std::string::npos);
}

TEST_CASE("mp3", "CBR 128/192/256/320: encode, decode, size, quality, stereo") {
    const auto x = music(10.0);
    const fs::path dir = tempDir("mp3cbr");
    double lastSnr = 0;
    for (int kbps : {128, 192, 256, 320}) {
        mp3::Options o;
        o.bitrateKbps = kbps;
        const fs::path f = dir / std::format("t{}.mp3", kbps);
        std::string err;
        REQUIRE_MSG_OK(mp3::encode(f, x, SR, o, &err), err);
        const double expectBytes = kbps * 1000.0 / 8.0 * 10.0;
        const double size = static_cast<double>(fs::file_size(f));
        CHECK_MSG(std::fabs(size - expectBytes) / expectBytes < 0.05, std::format("{} kbps: {} bytes vs {}", kbps, size, expectBytes));
        AudioData d;
        REQUIRE(readAudioFile(f, d, &err));
        CHECK(d.numChannels == 2);
        CHECK(d.sampleRate == SR);
        CHECK_MSG(d.numFrames == static_cast<int64_t>(x[0].size()), std::format("{} frames (gapless)", d.numFrames));
        auto [delay, snrL] = alignedSnr(x[0], d.channels[0]);
        auto [delayR, snrR] = alignedSnr(x[1], d.channels[1]);
        CHECK_MSG(snrL > 20.0 && snrR > 20.0, std::format("{} kbps SNR {:.1f}/{:.1f} dB (delay {})", kbps, snrL, snrR, delay));
        CHECK(delay == 0);                       // sample-accurate after gapless trim
        CHECK(delayR == 0);
        CHECK(snrL + 3.0 > lastSnr);              // quality does not drop with higher bitrate
        lastSnr = std::max(lastSnr, snrL);
    }
}

TEST_CASE("mp3", "VBR, mono, metadata (ID3v2 + ID3v1)") {
    const fs::path dir = tempDir("mp3meta");
    std::string err;
    // VBR smaller than 320 CBR for this material, still decodes cleanly
    mp3::Options v;
    v.vbr = true;
    v.vbrQuality = 4;
    REQUIRE_MSG_OK(mp3::encode(dir / "vbr.mp3", music(8.0), SR, v, &err), err);
    mp3::Options c320;
    REQUIRE(mp3::encode(dir / "cbr.mp3", music(8.0), SR, c320, &err));
    CHECK(fs::file_size(dir / "vbr.mp3") < fs::file_size(dir / "cbr.mp3"));
    AudioData d;
    REQUIRE(readAudioFile(dir / "vbr.mp3", d, &err));
    CHECK(alignedSnr(music(8.0)[0], d.channels[0]).second > 18.0);
    // mono
    mp3::Options m;
    m.bitrateKbps = 128;
    REQUIRE(mp3::encode(dir / "mono.mp3", music(4.0, 1), SR, m, &err));
    AudioData dm;
    REQUIRE(readAudioFile(dir / "mono.mp3", dm, &err));
    CHECK(dm.numChannels == 1);
    // metadata
    mp3::Options t;
    t.bitrateKbps = 192;
    t.title = "RoY Test Title";
    t.artist = "RoY Artist";
    t.album = "RoY Album";
    t.year = "2026";
    t.comment = "made with RoY Studio";
    t.track = "7";
    t.genre = "Hip-Hop";
    REQUIRE(mp3::encode(dir / "tags.mp3", music(2.0), SR, t, &err));
    const std::string b = fileBytes(dir / "tags.mp3");
    CHECK(b.rfind("ID3", 0) == 0); // ID3v2 at the start
    for (const char* frame : {"TIT2", "TPE1", "TALB", "TYER", "COMM", "TRCK", "TCON"}) CHECK_MSG(b.find(frame) != std::string::npos, frame);
    for (const char* text : {"RoY Test Title", "RoY Artist", "RoY Album", "2026", "made with RoY Studio"}) CHECK_MSG(b.find(text) != std::string::npos, text);
    REQUIRE(b.size() > 128);
    CHECK(b.substr(b.size() - 128, 3) == "TAG"); // ID3v1 at the end
    AudioData dt;
    REQUIRE(readAudioFile(dir / "tags.mp3", dt, &err)); // tags do not break decoding
    CHECK_MSG(dt.numFrames == 2 * SR, std::format("{} frames", dt.numFrames));
}

TEST_CASE("mp3", "long file: 10 minutes stereo") {
    const fs::path dir = tempDir("mp3long");
    const auto x = music(600.0);
    mp3::Options o;
    o.bitrateKbps = 320;
    o.quality = 5;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE_MSG_OK(mp3::encode(dir / "long.mp3", x, SR, o, &err), err);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("       10 min @320 kbps encoded in %.1f s (%.0fx realtime)\n", secs, 600.0 / secs);
    CHECK(secs < 120.0);
    AudioData d;
    REQUIRE(readAudioFile(dir / "long.mp3", d, &err));
    CHECK(d.numFrames == static_cast<int64_t>(x[0].size()));
    CHECK_NEAR(static_cast<double>(fs::file_size(dir / "long.mp3")), 320000.0 / 8 * 600, 320000.0 / 8 * 600 * 0.02);
}

TEST_CASE("mp3", "error cases: bitrate, sample rate, unwritable path, no overwrite, library missing") {
    const fs::path dir = tempDir("mp3err");
    std::string err;
    mp3::Options bad;
    bad.bitrateKbps = 100;
    CHECK(!mp3::encode(dir / "a.mp3", music(1.0), SR, bad, &err));
    CHECK(err.find("bitrate") != std::string::npos);
    mp3::Options o;
    CHECK(!mp3::encode(dir / "b.mp3", music(1.0), 96000, o, &err));
    CHECK(err.find("96000") != std::string::npos);
    CHECK(!mp3::encode(dir / "no_such_dir" / "c.mp3", music(1.0), SR, o, &err));
    CHECK(!fs::exists(dir / "no_such_dir"));
    REQUIRE(mp3::encode(dir / "d.mp3", music(1.0), SR, o, &err));
    const auto size = fs::file_size(dir / "d.mp3");
    CHECK(!mp3::encode(dir / "d.mp3", music(3.0), SR, o, &err)); // never overwrite
    CHECK(fs::file_size(dir / "d.mp3") == size);
    for (auto& e : fs::directory_iterator(dir)) CHECK_MSG(e.path().extension() != ".partial", e.path().string()); // no leftovers
    // A copy of roy_cli without the encoder library next to it reports a clear error.
    const fs::path cli = fs::path(ROY_PLUGIN_HOST_EXE).parent_path() /
#ifdef _WIN32
                         "roy_cli.exe";
#else
                         "roy_cli";
#endif
    if (fs::exists(cli)) {
        const fs::path lone = dir / "lone";
        fs::create_directories(lone);
        fs::copy_file(cli, lone / cli.filename());
        ChildProcess p;
        REQUIRE(p.start((lone / cli.filename()).string(), {"mp3-check"}));
        const std::string out = p.readAll(20000);
        p.wait(5000);
#ifdef _WIN32
        // Windows may still find a system-wide libmp3lame.dll; only check the report format
        CHECK(out.find("\"available\"") != std::string::npos);
#else
        CHECK_MSG(out.find("\"available\": false") != std::string::npos || out.find("libmp3lame.so.0") == std::string::npos, out);
        CHECK(p.exitCode() != 0 || out.find("\"available\": true") != std::string::npos);
#endif
    }
}

TEST_CASE("mp3", "Export command: MP3 with metadata from a project, 96 kHz project is resampled") {
    registerBuiltinProcessors();
    for (double projectRate : {48000.0, 96000.0}) {
        AudioEngine engine;
        engine.prepare(projectRate, 512);
        ProjectRuntime rt(engine);
        Project p = makeNewProject("Mp3 Song", projectRate, 120.0);
        auto d = makeSine(projectRate, 440.0, 3.0, 0.5f);
        const std::string asset = addMemoryAsset(p, rt, d);
        const std::string tid = addTrack(p, TrackType::Audio, "A").id;
        addClip(*p.findTrack(tid), asset, 0, 6.0);
        UndoManager undo(p);
        CommandRegistry reg;
        registerCoreCommands(reg);
        const fs::path dir = tempDir(std::format("mp3cmd{}", static_cast<int>(projectRate)));
        CommandContext ctx{p, undo, &rt, dir};
        REQUIRE_MSG_OK(reg.execute(ctx, "Export", {{"format", "mp3"}, {"bitrate", 192}, {"tailSeconds", 0.0},
                                                   {"metadata", {{"title", "Hook"}, {"artist", "RoY"}, {"track", "1"}}}}),
                       ctx.error);
        const fs::path f = ctx.result["files"][0]["path"].get<std::string>();
        CHECK(f.extension() == ".mp3");
        AudioData dd;
        std::string err;
        REQUIRE(readAudioFile(f, dd, &err));
        CHECK(dd.sampleRate == 48000); // 96 kHz projects are resampled for MP3
        const std::string b = fileBytes(f);
        CHECK(b.find("Hook") != std::string::npos);
        if (projectRate > 48000) CHECK(!ctx.result["warnings"].empty());
        // 440 Hz survives: spectral peak
        std::vector<float> seg(dd.channels[0].begin() + 20000, dd.channels[0].begin() + 20000 + 16384);
        auto mag = dsp::magnitudeSpectrum(seg.data(), 16384);
        size_t pk = 1;
        for (size_t i = 2; i < mag.size(); ++i)
            if (mag[i] > mag[pk]) pk = i;
        CHECK_NEAR(pk * 48000.0 / 16384.0, 440.0, 4.0);
    }
}
