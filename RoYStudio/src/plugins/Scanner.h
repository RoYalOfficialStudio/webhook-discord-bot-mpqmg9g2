#pragma once
// Plugin scanner + database.
//
// * Every CLAP module is scanned OUT OF PROCESS (roy_plugin_host --scan) with a
//   timeout. A module that crashes or hangs the scanner is QUARANTINED: it is
//   recorded as failed and skipped on later scans until the user retries it.
//   Plugin files are never moved, modified or deleted.
// * VST3 bundles are detected and described from moduleinfo.json when present;
//   loading VST3 needs the Steinberg VST3 SDK, which this build does not bundle,
//   so they are listed with status "unsupported" (honest, not hidden).
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace roy::plugins {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct PluginRecord {
    std::string typeId;   // "clap:<path>|<id>", "vst3:<path>|<class id or name>", or "file:<path>" for module-level failures
    std::string format;   // clap | vst3
    std::string path;
    std::string id, name, vendor, version;
    std::string category; // instrument | effect | unknown
    std::vector<std::string> features;
    std::string arch;     // x86_64, x86, arm64, unknown
    std::string status;   // ok | failed | crashed | timeout | unsupported | wrong_arch
    std::string error;
    int paramCount = 0;
    std::string scannedAt;
    double scanMs = 0.0;
    int64_t fileSize = 0;
    int64_t fileTime = 0;
    std::string duplicateOf; // typeId of the first record with the same plugin id

    json toJson() const;
    static PluginRecord fromJson(const json& j);
};

class PluginDatabase {
public:
    bool load(const fs::path& file, std::string* error = nullptr);
    bool save(const fs::path& file, std::string* error = nullptr) const;

    std::vector<PluginRecord> records;
    std::set<std::string> blacklist;   // module paths
    std::set<std::string> quarantine;  // module paths that crashed/hung the scanner
    std::set<std::string> favorites;   // typeIds
    std::deque<std::string> recent;    // typeIds, most recent first

    // ---- views -----------------------------------------------------------
    std::vector<const PluginRecord*> installed() const;   // loadable (status ok), incl. blacklisted
    std::vector<const PluginRecord*> available() const;   // ok and not blacklisted - offered in the browser
    std::vector<const PluginRecord*> failed() const;      // failed/crashed/timeout/wrong_arch/unsupported
    std::vector<const PluginRecord*> blacklisted() const;
    std::vector<const PluginRecord*> favoriteList() const;
    std::vector<const PluginRecord*> recentList() const;
    std::vector<const PluginRecord*> instruments() const;
    std::vector<const PluginRecord*> effects() const;
    std::vector<const PluginRecord*> duplicates() const;
    json view(const std::string& name) const; // INSTALLED, AVAILABLE, FAILED, BLACKLISTED, FAVORITES, RECENT, INSTRUMENTS, EFFECTS

    const PluginRecord* find(const std::string& typeId) const;
    void setBlacklisted(const std::string& path, bool on);
    void releaseQuarantine(const std::string& path);
    void setFavorite(const std::string& typeId, bool on);
    void markUsed(const std::string& typeId, size_t maxRecent = 20);
    bool isBlacklisted(const std::string& path) const { return blacklist.count(path) != 0; }
    void removeRecordsForPath(const std::string& path);
};

struct ScanOptions {
    std::vector<fs::path> paths;       // files or folders (searched recursively)
    int timeoutMs = 20000;             // per module
    bool force = false;                // rescan unchanged modules
    bool retryQuarantined = false;
    std::string hostExe;               // default: plugins::hostExecutable()
};

struct ScanReport {
    int modulesFound = 0, scanned = 0, skippedUnchanged = 0, skippedBlacklisted = 0, skippedQuarantined = 0;
    int pluginsOk = 0, failed = 0, crashed = 0, timeouts = 0, unsupported = 0, duplicates = 0;
    double seconds = 0.0;
    std::vector<std::string> messages;
    json toJson() const;
};

ScanReport scanPlugins(PluginDatabase& db, const ScanOptions& options);

// Standard plugin folders for the current OS (existing or not).
std::vector<fs::path> defaultPluginPaths();
// "x86_64", "x86", "arm64", "unknown" from the ELF/PE/Mach-O header.
std::string binaryArch(const fs::path& file);
std::string hostArch();
// Parses VST3 moduleinfo.json leniently (comments, trailing commas).
json parseLenientJson(const std::string& text);

} // namespace roy::plugins
