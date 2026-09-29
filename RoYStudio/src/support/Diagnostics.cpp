#include "support/Diagnostics.h"
#include "core/Files.h"
#include "plugins/Scanner.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace roy::support {

namespace fs = std::filesystem;

namespace {
std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
}

std::string readTail(const fs::path& file, int lines) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(f, line)) {
        tail.push_back(line);
        if (static_cast<int>(tail.size()) > lines) tail.pop_front();
    }
    std::string out;
    for (auto& l : tail) out += l + "\n";
    return out;
}

std::string readHead(const fs::path& file, size_t maxBytes) {
    std::ifstream f(file, std::ios::binary);
    std::string s(maxBytes, '\0');
    f.read(s.data(), static_cast<std::streamsize>(maxBytes));
    s.resize(static_cast<size_t>(f.gcount()));
    if (s.size() == maxBytes) s += "\n[... truncated]\n";
    return s;
}

std::string fence(const std::string& body) { return "```\n" + body + (body.empty() || body.back() == '\n' ? "" : "\n") + "```\n"; }
} // namespace

std::vector<std::string> systemInfo() {
    std::vector<std::string> v;
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr; fn && fn(&vi) == 0)
        v.push_back(std::format("OS: Windows {}.{} build {}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber));
    else
        v.push_back("OS: Windows (version unknown)");
    using WineVersionFn = const char*(CDECL*)();
    if (auto wine = ntdll ? reinterpret_cast<WineVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version"))) : nullptr)
        v.push_back(std::format("Wine: {} (not a native Windows installation)", wine()));
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        v.push_back(std::format("Memory: {:.1f} GB total, {:.1f} GB free", ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0));
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    v.push_back(std::format("CPU architecture: {}", si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                                                   : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64"
                                                                                                                 : "other"));
#else
    utsname u{};
    if (uname(&u) == 0) v.push_back(std::format("OS: {} {} ({})", u.sysname, u.release, u.machine));
    std::ifstream mi("/proc/meminfo");
    std::string line;
    while (std::getline(mi, line))
        if (line.rfind("MemTotal:", 0) == 0 || line.rfind("MemAvailable:", 0) == 0) v.push_back("Memory " + line);
#endif
    v.push_back(std::format("CPU threads: {}", std::thread::hardware_concurrency()));
    v.push_back(std::format("RoY version: {}", ROY_VERSION_STRING));
    return v;
}

std::string sanitize(const std::string& text) {
    std::string s = text;
#ifdef _WIN32
    std::string home = env("USERPROFILE");
    const std::string user = env("USERNAME");
#else
    std::string home = env("HOME");
    const std::string user = env("USER");
#endif
    if (home.size() > 3) { // never replace "/" or "C:\"
        std::string alt = home;
        std::replace(alt.begin(), alt.end(), '\\', '/');
        replaceAll(s, home, "~");
        replaceAll(s, alt, "~");
        std::replace(alt.begin(), alt.end(), '/', '\\');
        replaceAll(s, alt, "~");
    }
    if (user.size() >= 2) { // only as a path component ("C:\Users\<user>\", "/home/<user>/")
        replaceAll(s, "\\" + user + "\\", "\\<user>\\");
        replaceAll(s, "/" + user + "/", "/<user>/");
    }
    return s;
}

std::string buildReport(const DiagnosticsInput& in) {
    std::string md = std::format("# {} diagnostics report\n\ncreated: {}\n\n", in.program, files::nowIso8601());
    md += "> Review this file before you share it. It contains no project audio, no passwords and no tokens;\n"
          "> your home folder and user name are replaced by `~` / `<user>`. Project names and file paths can\n"
          "> appear in the log section - delete lines you do not want to share.\n\n";
    md += "## System\n\n";
    for (auto& l : systemInfo()) md += "* " + l + "\n";
    md += "\n## Audio\n\n";
    if (in.audioLines.empty()) md += "* (not available)\n";
    for (auto& l : in.audioLines) md += "* " + l + "\n";
    md += "\n## MIDI inputs\n\n";
    if (in.midiLines.empty()) md += "* none\n";
    for (auto& l : in.midiLines) md += "* " + l + "\n";
    if (!in.sessionLines.empty()) {
        md += "\n## Session\n\n";
        for (auto& l : in.sessionLines) md += "* " + l + "\n";
    }

    // plugins
    md += "\n## Plugins (last scan)\n\n";
    plugins::PluginDatabase db;
    const fs::path dbFile = in.userDataDir / "plugins.json";
    std::string err;
    if (!fs::exists(dbFile)) {
        md += "no plugin scan yet\n";
    } else if (!db.load(dbFile, &err)) {
        md += "plugin database unreadable: " + err + "\n";
    } else {
        std::map<std::string, int> byStatus;
        for (auto& r : db.records) ++byStatus[r.status + " " + r.format];
        for (auto& [k, n] : byStatus) md += std::format("* {}: {}\n", k, n);
        md += std::format("* quarantined modules: {}, blacklisted modules: {}\n", db.quarantine.size(), db.blacklist.size());
        const auto failed = db.failed();
        if (!failed.empty()) {
            md += "\n| Plugin / file | Format | Status | Reason |\n|---|---|---|---|\n";
            auto cell = [](std::string v) { // table-safe: no column breaks, one line
                std::replace(v.begin(), v.end(), '|', '/');
                std::replace(v.begin(), v.end(), '\n', ' ');
                std::replace(v.begin(), v.end(), '\r', ' ');
                return v;
            };
            for (auto* r : failed)
                md += std::format("| {} | {} | {} | {} |\n", cell(r->name.empty() ? r->path : r->name + " (" + r->path + ")"), cell(r->format),
                                  cell(r->status), cell(r->error));
        }
        if (!db.quarantine.empty()) {
            md += "\nQuarantined (crashed or hung the scanner):\n";
            for (auto& q : db.quarantine) md += "* " + q + "\n";
        }
    }

    // crash reports
    md += "\n## Crash reports\n\n";
    const fs::path crashDir = in.userDataDir / "CrashReports";
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(crashDir, ec))
        if (e.is_regular_file(ec)) entries.push_back(e);
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) > b.last_write_time(e2);
    });
    if (entries.empty()) md += "none\n";
    int shown = 0;
    for (auto& e : entries) {
        const std::string name = e.path().filename().string();
        if (name.ends_with(".unseen")) continue;
        md += std::format("* `{}` ({} bytes)\n", name, e.file_size(ec));
        if (name.ends_with(".txt") && shown < in.crashReportsIncluded) {
            ++shown;
            md += fence(readHead(e.path(), 8192));
        }
    }
    if (std::any_of(entries.begin(), entries.end(), [](auto& e) { return e.path().extension() == ".dmp"; }))
        md += "\nMinidumps (.dmp) are not embedded; attach them separately if a developer asks for them.\n";

    // log
    md += std::format("\n## Log (last {} lines of {})\n\n", in.logTailLines, in.logFileName);
    const std::string log = readTail(in.userDataDir / in.logFileName, in.logTailLines);
    md += log.empty() ? "log file not found\n" : fence(log);
    return sanitize(md);
}

fs::path writeReport(const DiagnosticsInput& in, const fs::path& file, std::string* error) {
    fs::path out = file;
    if (out.empty()) {
        std::string ts = files::nowIso8601();
        std::replace(ts.begin(), ts.end(), ':', '-');
        out = in.userDataDir / "Diagnostics" / ("RoY_Diagnostics_" + ts + ".md");
    }
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    if (!files::atomicWrite(out, buildReport(in), error)) return {};
    return out;
}

} // namespace roy::support
