#pragma once
// SYSTEM CHECK: quick PASS / WARNING / FAIL overview of everything RoY needs on this machine.
// Collects only technical facts (no user name, no file contents).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace roy::support {

struct SystemFacts {
    std::string os;          // "Windows 10.0 build 22631", "Linux 6.8 (x86_64)"
    bool windows = false;
    unsigned windowsBuild = 0;
    bool wine = false;
    bool x64 = false;
    unsigned cpuThreads = 0;
    double ramGB = 0.0;
};
SystemFacts systemFacts();

struct CheckItem {
    std::string name;
    std::string status; // PASS | WARNING | FAIL
    std::string detail;
};

struct SystemCheckInput {
    bool audioRunning = false;
    std::string audioBackend;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int outputDevices = 0;
    int inputChannels = 0;       // 0 = no input open
    uint64_t overloads = 0;      // callbacks over budget since the last audio change
    int midiInputs = 0;
    std::filesystem::path pluginHost;
    std::filesystem::path projectsDir;
};

std::vector<CheckItem> runSystemCheck(const SystemCheckInput& in);
// "PASS" if all pass, "WARNING" if any warning, "FAIL" if any failure.
std::string overall(const std::vector<CheckItem>& items);
std::string systemCheckMarkdown(const std::vector<CheckItem>& items);

} // namespace roy::support
