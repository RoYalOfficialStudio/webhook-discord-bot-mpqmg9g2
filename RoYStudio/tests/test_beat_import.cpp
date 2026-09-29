// IMPORT BEAT: a bought / downloaded MP3 or WAV beat on its own track from bar 1, tempo and key
// from the file name (or detected), one undo step, the file copied into the project.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "arrange/BeatImport.h"
#include "commands/Commands.h"
#include "core/Math.h"
#include "export/Mp3Encoder.h"
#include "io/AudioFile.h"

#include <cmath>
#include <format>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 44100.0;

// 8 s "trap beat" at 140 BPM in A minor: kick on every beat + A minor pad (A C E).
std::vector<std::vector<float>> beat140(double seconds = 8.0) {
    const size_t n = static_cast<size_t>(seconds * SR);
    std::vector<std::vector<float>> x(2, std::vector<float>(n));
    const double beatSec = 60.0 / 140.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / SR;
        const double tb = std::fmod(t, beatSec);
        const double kick = std::exp(-tb * 25.0) * std::sin(kTwoPi * (50.0 + 80.0 * std::exp(-tb * 40.0)) * tb);
        const double pad = 0.08 * (std::sin(kTwoPi * 220.0 * t) + std::sin(kTwoPi * 261.63 * t) + std::sin(kTwoPi * 329.63 * t));
        x[0][i] = x[1][i] = static_cast<float>(0.6 * kick + pad);
    }
    return x;
}

struct Song {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("song", 48000.0, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    fs::path dir;
    explicit Song(const std::string& name) {
        engine.prepare(48000.0, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        dir = tempDir(name);
        fs::create_directories(dir / "Audio");
        rt.setProjectDirectory(dir);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt, dir});
        p.key = Key{0, ScaleType::Major};
    }
    bool run(const std::string& id, const json& a) {
        const bool ok = reg.execute(*ctx, id, a);
        if (!ok) std::fprintf(stderr, "%s failed: %s\n", id.c_str(), ctx->error.c_str());
        return ok;
    }
};
} // namespace

TEST_CASE("beatimport", "tempo and key from typical beat file names") {
    using namespace beatimport;
    CHECK(bpmFromFileName("Night Trap 140 BPM Am").value_or(0) == 140.0);
    CHECK(bpmFromFileName("dark_drill_beat_142bpm").value_or(0) == 142.0);
    CHECK(bpmFromFileName("BPM 95 boom bap").value_or(0) == 95.0);
    CHECK(bpmFromFileName("[FREE] Travis Type Beat 2024 - Sicko").has_value() == false); // 2024 is no tempo
    CHECK(bpmFromFileName("beat 87.5 bpm").value_or(0) == 87.5);
    auto k = keyFromFileName("Night Trap 140 BPM Am");
    REQUIRE(k.has_value());
    CHECK(k->root == 9);
    CHECK(k->scale == ScaleType::NaturalMinor);
    k = keyFromFileName("Pain (F#m) 150bpm");
    REQUIRE(k.has_value());
    CHECK(k->root == 6);
    k = keyFromFileName("Summer_Eb_Major_100BPM");
    REQUIRE(k.has_value());
    CHECK(k->root == 3);
    CHECK(k->scale == ScaleType::Major);
    k = keyFromFileName("Cold - C# min - 130bpm");
    REQUIRE(k.has_value());
    CHECK(k->root == 1);
    CHECK(!keyFromFileName("A Beat For You").has_value());      // bare letter
    CHECK(!keyFromFileName("Emotional Drill Beat").has_value()); // "Em" inside a word
    CHECK(!keyFromFileName("Dmx type beat").has_value());
    CHECK(!keyFromFileName("Studio 8AM session").has_value());   // AM is not A minor
    CHECK(isImportableAudio("x.MP3") && isImportableAudio("x.wav") && !isImportableAudio("x.txt"));
}

TEST_CASE("beatimport", "MP3 beat: analysed, imported on its own track at bar 1 with song tempo + key, one undo step") {
    Song s("beat_mp3");
    const fs::path src = tempDir("beat_mp3_src") / "Night Trap 140 BPM Am.mp3";
    mp3::Options o;
    o.bitrateKbps = 320;
    std::string err;
    REQUIRE_MSG_OK(mp3::encode(src, beat140(), static_cast<int>(SR), o, &err), err);
    const auto info = beatimport::analyzeBeatFile(src);
    REQUIRE_MSG_OK(info.ok, info.error);
    CHECK_NEAR(info.seconds, 8.0, 0.01);
    CHECK(info.suggestedBpm() == 140.0);
    REQUIRE(info.suggestedKey().has_value());
    CHECK(info.suggestedKey()->name() == "A Minor");
    // the audio itself: kick every 60/140 s -> 140 or 70 BPM
    CHECK_MSG(std::fabs(info.bpmDetected - 140.0) < 2.0 || std::fabs(info.bpmDetected - 70.0) < 1.5, std::format("{:.1f} BPM", info.bpmDetected));

    const size_t u0 = s.undo.undoCount();
    const size_t tracks0 = s.p.tracks.size();
    REQUIRE(s.run("ImportBeat", {{"path", src.string()}, {"bpm", 140.0}, {"key", "A Minor"}}));
    CHECK(s.undo.undoCount() == u0 + 1);
    REQUIRE(s.p.tracks.size() == tracks0 + 1);
    const Track& t = s.p.tracks.back();
    CHECK(t.type == TrackType::Audio);
    CHECK(t.name == "Night Trap 140 BPM Am");
    CHECK(t.role == "beat");
    REQUIRE(t.audioClips.size() == 1);
    CHECK(t.audioClips[0].startBeat == 0.0);
    CHECK_NEAR(t.audioClips[0].lengthBeats, 8.0 * 140.0 / 60.0, 0.05); // whole beat, at the new tempo
    CHECK_NEAR(s.p.tempo.tempoAt(0), 140.0, 1e-9);
    CHECK(s.p.key.name() == "A Minor");
    CHECK(fs::exists(s.dir / "Audio" / src.filename())); // copied into the project
    CHECK(fs::exists(src));                                // original untouched
    // it plays
    REQUIRE(s.rt.rebuild(s.p));
    s.engine.transport().seek(0);
    s.engine.transport().play();
    const auto out = render(s.engine, 48000);
    CHECK(peak(out[0]) > 0.1f);
    // one Ctrl+Z: no track, old tempo and key
    REQUIRE(s.undo.undo());
    CHECK(s.p.tracks.size() == tracks0);
    CHECK_NEAR(s.p.tempo.tempoAt(0), 120.0, 1e-9);
    CHECK(s.p.key.name() == "C Major");
}

TEST_CASE("beatimport", "WAV beat without tempo/key keeps the song settings; onto an existing track; bad files refused") {
    Song s("beat_wav");
    const fs::path src = tempDir("beat_wav_src") / "my beat.wav";
    REQUIRE(writeWavFile(src, beat140(4.0), SR, SampleFormat::Pcm24));
    const auto info = beatimport::analyzeBeatFile(src);
    REQUIRE(info.ok);
    CHECK(!info.bpmFromName.has_value());
    REQUIRE(s.run("ImportBeat", {{"path", src.string()}}));
    CHECK_NEAR(s.p.tempo.tempoAt(0), 120.0, 1e-9);
    CHECK(s.p.key.name() == "C Major");
    CHECK_NEAR(s.p.tracks.back().audioClips[0].lengthBeats, 4.0 * 120.0 / 60.0, 0.05);
    // onto an existing audio track at bar 5
    const std::string tid = s.p.tracks.back().id;
    REQUIRE(s.run("ImportBeat", {{"path", src.string()}, {"trackId", tid}, {"startBeat", 16.0}}));
    CHECK(s.p.findTrack(tid)->audioClips.size() == 2);
    CHECK(fs::exists(s.dir / "Audio" / "my beat.wav"));
    CHECK(fs::exists(s.dir / "Audio" / "my beat_0001.wav")); // never overwritten
    // not audio / missing / bad tempo: refused, nothing changes
    const fs::path txt = src.parent_path() / "readme.txt";
    REQUIRE(files::atomicWrite(txt, "not audio"));
    const size_t u = s.undo.undoCount();
    CHECK(!s.run("ImportBeat", {{"path", txt.string()}}));
    CHECK(!s.run("ImportBeat", {{"path", (src.parent_path() / "missing.mp3").string()}}));
    CHECK(!s.run("ImportBeat", {{"path", src.string()}, {"bpm", 5000.0}}));
    CHECK(!s.run("ImportBeat", {{"path", src.string()}, {"key", "H Dur"}}));
    CHECK(s.undo.undoCount() == u);
    CHECK(!beatimport::analyzeBeatFile(txt).ok);
    // file names with umlauts / accents (typical for downloads) work on Windows too
    const fs::path umlaut = src.parent_path() / fs::path(u8"Beat f\u00fcr dich \u00e9t\u00e9 140 BPM Am.wav");
    std::string werr;
    REQUIRE_MSG_OK(writeWavFile(umlaut, beat140(2.0), SR, SampleFormat::Pcm16, true, &werr), werr);
    const auto ui = beatimport::analyzeBeatFile(umlaut);
    REQUIRE_MSG_OK(ui.ok, ui.error);
    CHECK(ui.suggestedBpm() == 140.0);
    REQUIRE(s.run("ImportBeat", {{"path", umlaut.string()}}));
    CHECK(fs::exists(s.dir / "Audio" / umlaut.filename()));
}
