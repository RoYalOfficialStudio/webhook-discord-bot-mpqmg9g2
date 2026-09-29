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
