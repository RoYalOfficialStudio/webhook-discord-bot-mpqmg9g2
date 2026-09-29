// SUPPORT: crash handler of RoY's own programs (real crashes in a child process: invalid
// memory access, abort, std::terminate -> report + marker (+ minidump on Windows), the process
// still ends as crashed) and the diagnostics report (sections, plugin failures, crash reports,
// log tail, home folder / user name anonymised).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "core/CrashHandler.h"
#include "core/Files.h"
#include "core/Process.h"
#include "plugins/Scanner.h"
#include "support/AppSettings.h"
#include "support/Diagnostics.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
std::string readAll(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}
} // namespace

TEST_CASE("support", "crash handler: invalid access / abort / terminate in a child -> report + marker, still a crash") {
    for (const std::string kind : {"segv", "abort", "terminate"}) {
        const fs::path dir = tempDir("crash_" + kind);
        ChildProcess p;
        std::string err;
        REQUIRE_MSG_OK(p.start(ROY_CLI_EXE, {"crash-test", dir.string(), kind}, &err), err);
        REQUIRE(p.wait(30000));
        CHECK_MSG(p.crashed() || p.exitCode() != 0, kind); // the parent still sees a failure
        std::vector<fs::path> txt, dmp;
        for (auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".txt") txt.push_back(e.path());
            if (e.path().extension() == ".dmp") dmp.push_back(e.path());
        }
        REQUIRE_MSG_OK(txt.size() == 1, kind);
        const std::string report = readAll(txt[0]);
        CHECK(report.find("roy_cli crash report") != std::string::npos);
        CHECK(report.find("version: " ROY_VERSION_STRING) != std::string::npos);
        CHECK(report.find("error: ") != std::string::npos);
        if (kind == "segv") {
#ifdef _WIN32
            CHECK(report.find("ACCESS_VIOLATION") != std::string::npos);
            CHECK(report.find("module: roy_cli.exe") != std::string::npos);
            REQUIRE(dmp.size() == 1);
            CHECK(fs::file_size(dmp[0]) > 1000);
#else
            CHECK(report.find("SIGSEGV") != std::string::npos);
            CHECK(report.find("backtrace:") != std::string::npos);
#endif
        } else {
            CHECK((report.find("abort") != std::string::npos || report.find("SIGABRT") != std::string::npos));
        }
        // the next start sees it exactly once
        const auto unseen = crash::takeUnseenReports(dir.string());
        REQUIRE(unseen.size() == 1);
        CHECK(fs::path(unseen[0]).filename() == txt[0].filename());
        CHECK(crash::takeUnseenReports(dir.string()).empty());
        CHECK(fs::exists(txt[0])); // the report itself stays
    }
}

TEST_CASE("support", "diagnostics report: sections, plugin failures, crash reports, bounded log tail, anonymised") {
    const fs::path user = tempDir("diag_user");
    const std::string home = homeDir();
    REQUIRE(home.size() > 3);
    // plugin database with one good and one crashed plugin
    plugins::PluginDatabase db;
    plugins::PluginRecord ok;
    ok.typeId = "clap:" + home + "/plugins/good.clap|good";
    ok.format = "clap";
    ok.path = home + "/plugins/good.clap";
    ok.name = "Good Synth";
    ok.status = "ok";
    plugins::PluginRecord bad = ok;
    bad.typeId = "file:" + home + "/plugins/bad.vst3";
    bad.format = "vst3";
    bad.path = home + "/plugins/bad.vst3";
    bad.name = "Bad | Plugin";
    bad.status = "crashed";
    bad.error = "plugin crashed or terminated the scanner\nwithout a result";
    db.records = {ok, bad};
    db.quarantine.insert(bad.path);
    REQUIRE(db.save(user / "plugins.json"));
    // a crash report and a log that mentions the home folder
    fs::create_directories(user / "CrashReports");
    { std::ofstream(user / "CrashReports" / "roy_studio_crash_1_2.txt") << "roy_studio crash report\nerror: SIGSEGV\n"; }
    { std::ofstream(user / "CrashReports" / "roy_studio_crash_1_2.txt.unseen"); }
    {
        std::ofstream log(user / "roy_studio.log");
        for (int i = 0; i < 1000; ++i) log << "line " << i << " opened " << home << "/Music/song.roy\n";
    }
    support::DiagnosticsInput in;
    in.userDataDir = user;
    in.audioLines = {"backend: null"};
    in.midiLines = {"Keyboard (open)"};
    in.logTailLines = 50;
    const std::string md = support::buildReport(in);
    for (const char* section : {"## System", "## Audio", "## MIDI inputs", "## Plugins (last scan)", "## Crash reports", "## Log (last 50 lines"})
        CHECK_MSG(md.find(section) != std::string::npos, section);
    CHECK(md.find("RoY version: " ROY_VERSION_STRING) != std::string::npos);
    CHECK(md.find("crashed vst3: 1") != std::string::npos);
    CHECK(md.find("ok clap: 1") != std::string::npos);
    CHECK(md.find("Bad / Plugin") != std::string::npos);                    // table-safe
    CHECK(md.find("scanner without a result") != std::string::npos);        // one line
    CHECK(md.find("roy_studio crash report") != std::string::npos);         // report embedded
    CHECK(md.find(".unseen") == std::string::npos);                          // markers are not listed
    CHECK(md.find("line 999 ") != std::string::npos);
    CHECK(md.find("line 949 ") == std::string::npos);                        // only the last 50 lines
    CHECK(md.find(home) == std::string::npos);                               // anonymised
    CHECK(md.find("~/Music/song.roy") != std::string::npos);
    // written atomically into <user>/Diagnostics
    std::string err;
    const fs::path out = support::writeReport(in, {}, &err);
    REQUIRE_MSG_OK(!out.empty(), err);
    CHECK(out.parent_path().filename() == "Diagnostics");
    CHECK(readAll(out).find("## Crash reports") != std::string::npos);
    // missing data is reported, never a failure
    support::DiagnosticsInput empty;
    empty.userDataDir = tempDir("diag_empty");
    const std::string md2 = support::buildReport(empty);
    CHECK(md2.find("no plugin scan yet") != std::string::npos);
    CHECK(md2.find("log file not found") != std::string::npos);
}

TEST_CASE("support", "sanitize: home folder in both slash styles and the user name as a path component") {
    const std::string home = homeDir();
    REQUIRE(home.size() > 3);
    std::string alt = home;
    std::replace(alt.begin(), alt.end(), '\\', '/');
    const std::string s = support::sanitize("a " + home + "/x b " + alt + "/y");
    CHECK(s.find(home) == std::string::npos);
    CHECK(s.find(alt) == std::string::npos);
    CHECK(s.find("~/x") != std::string::npos);
    CHECK(support::sanitize("RoY Studio roy_studio.log") == "RoY Studio roy_studio.log"); // plain words untouched
}

TEST_CASE("support", "app settings: roundtrip, tolerant loading, damaged file kept + defaults, unknown keys preserved") {
    const fs::path dir = tempDir("settings");
    const fs::path file = dir / "settings.json";
    // no file -> defaults, no note
    std::string note;
    auto s = support::loadSettings(file, &note);
    CHECK(note.empty());
    CHECK(!s.firstRunDone);
    CHECK(s.audio.bufferSize == 256);
    // roundtrip
    s.audio.backend = "wasapi";
    s.audio.outputDevice = "Speakers (USB Audio)";
    s.audio.inputDevice = "Mic (USB Audio)";
    s.audio.sampleRate = 44100.0;
    s.audio.bufferSize = 128;
    s.audio.enableInput = false;
    s.midiInputsOff = {"winmm:Pad Controller"};
    s.firstRunDone = true;
    s.unknown["futureFeature"] = {{"x", 1}};
    REQUIRE(support::saveSettings(file, s));
    auto back = support::loadSettings(file, &note);
    CHECK(note.empty());
    CHECK(back.audio.backend == "wasapi");
    CHECK(back.audio.outputDevice == "Speakers (USB Audio)");
    CHECK(back.audio.inputDevice == "Mic (USB Audio)");
    CHECK(back.audio.sampleRate == 44100.0);
    CHECK(back.audio.bufferSize == 128);
    CHECK(!back.audio.enableInput);
    REQUIRE(back.midiInputsOff.size() == 1);
    CHECK(back.midiInputsOff[0] == "winmm:Pad Controller");
    CHECK(back.firstRunDone);
    CHECK(back.unknown["futureFeature"]["x"] == 1); // newer versions' keys survive
    // tolerant: invalid values fall back to defaults field by field
    const auto t = support::settingsFromJson(json::parse(R"({"audio":{"backend":"evil","bufferSize":0,"sampleRate":"x","outputDevice":5},
                                                             "midi":{"inputsOff":[1,"ok"]},"firstRunDone":"yes"})"));
    CHECK(t.audio.backend == "auto");
    CHECK(t.audio.bufferSize == 256);
    CHECK(t.audio.sampleRate == 48000.0);
    CHECK(t.audio.outputDevice.empty());
    REQUIRE(t.midiInputsOff.size() == 1);
    CHECK(!t.firstRunDone);
    // damaged file: defaults + note, the damaged file is kept as a copy (never silently lost)
    { std::ofstream(file, std::ios::trunc) << "{\"audio\": {\"bufferSize\": 12"; }
    auto d = support::loadSettings(file, &note);
    CHECK(!note.empty());
    CHECK(d.audio.bufferSize == 256);
    bool kept = false;
    for (auto& e : fs::directory_iterator(dir)) kept |= e.path().filename().string().find("settings.json.corrupt") == 0;
    CHECK(kept);
}

TEST_CASE("support", "engine input peak meter: max of the device input, reset on read") {
    AudioEngine e;
    e.prepare(48000.0, 256);
    std::vector<float> in0(256, 0.0f), in1(256, 0.0f), o0(256), o1(256);
    in0[10] = -0.5f;
    in1[20] = 0.25f;
    const float* ins[2] = {in0.data(), in1.data()};
    float* outs[2] = {o0.data(), o1.data()};
    e.process(ins, 2, outs, 2, 256);
    CHECK_NEAR(e.inputPeak(0, false), 0.5f, 1e-6);
    CHECK_NEAR(e.inputPeak(0), 0.5f, 1e-6);
    CHECK(e.inputPeak(0) == 0.0f); // reset by the read
    CHECK_NEAR(e.inputPeak(1), 0.25f, 1e-6);
    e.process(nullptr, 0, outs, 2, 256); // no inputs: stays 0
    CHECK(e.inputPeak(0) == 0.0f);
}
