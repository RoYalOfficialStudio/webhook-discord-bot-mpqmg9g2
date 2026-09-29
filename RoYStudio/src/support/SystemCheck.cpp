#include "support/SystemCheck.h"
#include "core/Files.h"
#include "core/Process.h"
#include "export/Mp3Encoder.h"

#include <format>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

namespace roy::support {

namespace fs = std::filesystem;

SystemFacts systemFacts() {
    SystemFacts f;
    f.cpuThreads = std::thread::hardware_concurrency();
#ifdef _WIN32
    f.windows = true;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr; fn && fn(&vi) == 0) {
        f.windowsBuild = vi.dwBuildNumber;
        f.os = std::format("Windows {}.{} build {}{}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
                           vi.dwBuildNumber >= 22000 ? " (Windows 11)" : vi.dwMajorVersion >= 10 ? " (Windows 10)" : "");
    } else {
        f.os = "Windows (version unknown)";
    }
    f.wine = ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    f.x64 = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) f.ramGB = static_cast<double>(ms.ullTotalPhys) / 1073741824.0;
#else
    utsname u{};
    if (uname(&u) == 0) {
        f.os = std::format("{} {} ({})", u.sysname, u.release, u.machine);
        f.x64 = std::string(u.machine) == "x86_64";
    }
    std::ifstream mi("/proc/meminfo");
    std::string key;
    double kb = 0;
    while (mi >> key >> kb) {
        if (key == "MemTotal:") f.ramGB = kb / 1048576.0;
        mi.ignore(256, '\n');
    }
#endif
    return f;
}

std::vector<CheckItem> runSystemCheck(const SystemCheckInput& in) {
    std::vector<CheckItem> v;
    const SystemFacts f = systemFacts();
    // operating system
    if (f.windows && f.wine) v.push_back({"Windows version", "WARNING", f.os + " under Wine - not a native Windows installation"});
    else if (f.windows) v.push_back({"Windows version", f.windowsBuild >= 17763 ? "PASS" : "WARNING",
                                     f.os + (f.windowsBuild >= 17763 ? "" : " - Windows 10 1809 or newer recommended")});
    else v.push_back({"Operating system", "WARNING", f.os + " - development build (the test target is Windows 10/11)"});
    v.push_back({"x64", f.x64 ? "PASS" : "FAIL", f.x64 ? "64-bit" : "RoY needs a 64-bit x64 system"});
    v.push_back({"CPU", f.cpuThreads >= 4 ? "PASS" : f.cpuThreads >= 2 ? "WARNING" : "FAIL",
                 std::format("{} threads{}", f.cpuThreads, f.cpuThreads >= 4 ? "" : " - fewer plugins/tracks possible")});
    v.push_back({"RAM", f.ramGB >= 7.5 ? "PASS" : f.ramGB >= 3.5 ? "WARNING" : "FAIL", std::format("{:.1f} GB", f.ramGB)});
    // audio
    v.push_back({"Audio output", in.audioRunning ? "PASS" : "FAIL",
                 in.audioRunning ? in.audioBackend + std::format(" ({} output devices)", in.outputDevices)
                                 : "no audio device running - choose an output in the setup check / Audio menu"});
    v.push_back({"Audio input", in.inputChannels > 0 ? "PASS" : "WARNING",
                 in.inputChannels > 0 ? std::format("{} input channels", in.inputChannels)
                                      : "no input open - recording not possible (microphone, Windows privacy settings, input switched off?)"});
    if (!in.audioRunning) {
        v.push_back({"Sample rate", "WARNING", "audio is not running - no rate to check"});
        v.push_back({"Buffer", "WARNING", "audio is not running - no buffer to check"});
    } else {
    const bool commonRate = in.sampleRate == 44100.0 || in.sampleRate == 48000.0;
    v.push_back({"Sample rate", commonRate ? "PASS" : "WARNING",
                 std::format("{:.0f} Hz{}", in.sampleRate, commonRate ? "" : " - 44100 or 48000 Hz recommended (less CPU)")});
    std::string bufStatus = "PASS", bufDetail = std::format("{} samples ({:.1f} ms)", in.bufferSize,
                                                           in.sampleRate > 0 ? 1000.0 * in.bufferSize / in.sampleRate : 0.0);
    if (in.overloads > 0) {
        bufStatus = "WARNING";
        bufDetail += std::format(" - {} dropouts measured: choose a larger buffer (256 or 512)", in.overloads);
    } else if (in.bufferSize > 512) {
        bufStatus = "WARNING";
        bufDetail += " - high latency for recording/monitoring";
    }
    v.push_back({"Buffer", bufStatus, bufDetail});
    }
    v.push_back({"MIDI", in.midiInputs > 0 ? "PASS" : "WARNING",
                 in.midiInputs > 0 ? std::format("{} MIDI input(s)", in.midiInputs) : "no MIDI keyboard found (optional)"});
    // plugin host
    {
        std::error_code ec;
        std::string status = "FAIL", detail = "missing: " + in.pluginHost.filename().string() + " must be next to RoY Studio";
        if (fs::exists(in.pluginHost, ec)) {
            ChildProcess p;
            std::string err;
            if (p.start(in.pluginHost.string(), {"--version"}, &err)) {
                const std::string out = p.readAll(5000);
                p.wait(2000);
                if (out.find("RoYPluginHost") != std::string::npos) status = "PASS", detail = "plugin sandbox starts (" + out.substr(0, out.find('\n')) + ")";
                else detail = "plugin host did not answer: " + out.substr(0, 120);
            } else {
                detail = "plugin host cannot start: " + err;
            }
        }
        v.push_back({"Plugin host", status, detail});
    }
    {
        std::string why;
        const bool ok = mp3::available(&why);
        v.push_back({"MP3 encoder", ok ? "PASS" : "WARNING", ok ? "LAME found (roy_mp3lame)" : "MP3 export unavailable: " + why});
    }
    // project folder: write access + free space
    {
        std::error_code ec;
        fs::create_directories(in.projectsDir, ec);
        const fs::path probe = in.projectsDir / (".roy_write_test_" + files::newId());
        std::string err;
        const bool ok = files::atomicWrite(probe, "RoY write test", &err);
        fs::remove(probe, ec);
        v.push_back({"Project folder write access", ok ? "PASS" : "FAIL", ok ? "projects folder is writable" : "cannot write: " + err});
        const auto sp = fs::space(in.projectsDir, ec);
        if (!ec) {
            const double gb = static_cast<double>(sp.available) / 1073741824.0;
            v.push_back({"Free disk space", gb >= 2.0 ? "PASS" : gb >= 0.2 ? "WARNING" : "FAIL", std::format("{:.1f} GB free for projects", gb)});
        }
    }
    return v;
}

std::string overall(const std::vector<CheckItem>& items) {
    std::string r = "PASS";
    for (auto& i : items) {
        if (i.status == "FAIL") return "FAIL";
        if (i.status == "WARNING") r = "WARNING";
    }
    return r;
}

std::string systemCheckMarkdown(const std::vector<CheckItem>& items) {
    std::string md = "| Check | Result | Details |\n|---|---|---|\n";
    for (auto& i : items) md += std::format("| {} | **{}** | {} |\n", i.name, i.status, i.detail);
    md += "\nOverall: **" + overall(items) + "**\n";
    return md;
}

} // namespace roy::support
