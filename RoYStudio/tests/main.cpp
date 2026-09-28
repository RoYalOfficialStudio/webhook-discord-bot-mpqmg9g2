// Test runner: runs all registered tests, prints results, optionally writes a
// JUnit-style XML and a Markdown report (--report <dir>).
#include "TestFramework.h"
#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>

namespace roytest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

thread_local bool g_countAllocations = false;
thread_local long g_allocationCount = 0;

namespace {
int g_failedChecks = 0;
int g_totalChecks = 0;
std::vector<std::string> g_messages;
std::string g_currentTest;
} // namespace

void recordCheck(bool ok, const std::string& expr, const char* file, int line) {
    ++g_totalChecks;
    if (!ok) {
        ++g_failedChecks;
        std::string f = file;
        auto p = f.find_last_of("/\\");
        if (p != std::string::npos) f = f.substr(p + 1);
        g_messages.push_back(std::format("{}:{}: CHECK failed: {}", f, line, expr));
    }
}

int checksFailedInCurrentTest() { return g_failedChecks; }

std::filesystem::path tempDir(const std::string& name) {
    auto base = std::filesystem::temp_directory_path() / "roy_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    return base;
}

} // namespace roytest

// ---- allocation counting --------------------------------------------------
void* operator new(std::size_t n) {
    if (roytest::g_countAllocations) ++roytest::g_allocationCount;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (roytest::g_countAllocations) ++roytest::g_allocationCount;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '&': o += "&amp;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

int main(int argc, char** argv) {
    using namespace roytest;
    std::string filter, reportDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--report" && i + 1 < argc) reportDir = argv[++i];
        else if (a == "--list") {
            for (auto& t : registry()) std::printf("%s / %s\n", t.suite, t.name);
            return 0;
        } else filter = a;
    }
    roy::log::setLevel(roy::log::Level::Warn);
    struct Result { std::string suite, name; bool ok; double seconds; std::vector<std::string> messages; };
    std::vector<Result> results;
    int failed = 0;
    for (auto& t : registry()) {
        const std::string full = std::string(t.suite) + "/" + t.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        g_failedChecks = 0;
        g_messages.clear();
        g_currentTest = full;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        try {
            t.fn();
        } catch (const Failure& f) {
            ok = false;
            g_messages.push_back(f.message);
        } catch (const std::exception& e) {
            ok = false;
            g_messages.push_back(std::string("exception: ") + e.what());
        }
        ok = ok && g_failedChecks == 0;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %s (%.3fs)\n", ok ? " OK " : "FAIL", full.c_str(), secs);
        for (auto& m : g_messages) std::printf("       %s\n", m.c_str());
        std::fflush(stdout);
        if (!ok) ++failed;
        results.push_back({t.suite, t.name, ok, secs, g_messages});
    }
    std::printf("\n%zu tests, %d failed, %d checks total\n", results.size(), failed, g_totalChecks);

    if (!reportDir.empty()) {
        std::filesystem::create_directories(reportDir);
        std::ofstream x(std::filesystem::path(reportDir) / "roy_tests.xml");
        x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites tests=\"" << results.size() << "\" failures=\"" << failed << "\">\n";
        for (auto& r : results) {
            x << "  <testcase classname=\"" << xmlEscape(r.suite) << "\" name=\"" << xmlEscape(r.name) << "\" time=\"" << r.seconds << "\">";
            if (!r.ok) {
                x << "<failure message=\"failed\">";
                for (auto& m : r.messages) x << xmlEscape(m) << "\n";
                x << "</failure>";
            }
            x << "</testcase>\n";
        }
        x << "</testsuites>\n";
        std::ofstream md(std::filesystem::path(reportDir) / "roy_tests.md");
        md << "# RoY Studio test report\n\n" << results.size() << " tests, " << failed << " failed, " << g_totalChecks
           << " checks\n\n| Suite | Test | Result | Time (s) |\n|---|---|---|---|\n";
        for (auto& r : results)
            md << "| " << r.suite << " | " << r.name << " | " << (r.ok ? "PASS" : "**FAIL**") << " | " << std::format("{:.3f}", r.seconds) << " |\n";
        for (auto& r : results)
            if (!r.ok) {
                md << "\n## FAIL " << r.suite << "/" << r.name << "\n\n```\n";
                for (auto& m : r.messages) md << m << "\n";
                md << "```\n";
            }
    }
    return failed == 0 ? 0 : 1;
}
