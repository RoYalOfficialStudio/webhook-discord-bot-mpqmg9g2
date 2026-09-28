#include "plugins/Scanner.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Process.h"
#include "plugins/Sandbox.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>

namespace roy::plugins {

// ------------------------------------------------------------------ record
json PluginRecord::toJson() const {
    return {{"typeId", typeId}, {"format", format}, {"path", path}, {"id", id}, {"name", name}, {"vendor", vendor},
            {"version", version}, {"category", category}, {"features", features}, {"arch", arch}, {"status", status},
            {"error", error}, {"paramCount", paramCount}, {"scannedAt", scannedAt}, {"scanMs", scanMs}, {"fileSize", fileSize},
            {"fileTime", fileTime}, {"duplicateOf", duplicateOf}};
}

PluginRecord PluginRecord::fromJson(const json& j) {
    PluginRecord r;
    r.typeId = j.value("typeId", "");
    r.format = j.value("format", "");
    r.path = j.value("path", "");
    r.id = j.value("id", "");
    r.name = j.value("name", "");
    r.vendor = j.value("vendor", "");
    r.version = j.value("version", "");
    r.category = j.value("category", "unknown");
    if (j.contains("features") && j["features"].is_array())
        for (auto& f : j["features"]) r.features.push_back(f.get<std::string>());
    r.arch = j.value("arch", "unknown");
    r.status = j.value("status", "failed");
    r.error = j.value("error", "");
    r.paramCount = j.value("paramCount", 0);
    r.scannedAt = j.value("scannedAt", "");
    r.scanMs = j.value("scanMs", 0.0);
    r.fileSize = j.value("fileSize", int64_t{0});
    r.fileTime = j.value("fileTime", int64_t{0});
    r.duplicateOf = j.value("duplicateOf", "");
    return r;
}

// ------------------------------------------------------------------ database
bool PluginDatabase::load(const fs::path& file, std::string* error) {
    auto text = files::readAll(file);
    if (!text) {
        if (error) *error = "cannot read " + file.string();
        return false;
    }
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "plugin database is corrupt: " + file.string();
        return false;
    }
    records.clear();
    for (auto& r : j.value("records", json::array())) records.push_back(PluginRecord::fromJson(r));
    blacklist = j.value("blacklist", std::set<std::string>{});
    quarantine = j.value("quarantine", std::set<std::string>{});
    favorites = j.value("favorites", std::set<std::string>{});
    recent.clear();
    for (auto& r : j.value("recent", json::array())) recent.push_back(r.get<std::string>());
    return true;
}

bool PluginDatabase::save(const fs::path& file, std::string* error) const {
    json recs = json::array();
    for (auto& r : records) recs.push_back(r.toJson());
    json j = {{"format", "RoYStudio.PluginDatabase"}, {"version", 1}, {"records", recs}, {"blacklist", blacklist},
              {"quarantine", quarantine}, {"favorites", favorites}, {"recent", recent}};
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    return files::atomicWrite(file, j.dump(2), error);
}

namespace {
template <typename Pred>
std::vector<const PluginRecord*> select(const std::vector<PluginRecord>& v, Pred p) {
    std::vector<const PluginRecord*> out;
    for (auto& r : v)
        if (p(r)) out.push_back(&r);
    return out;
}
bool isFailure(const std::string& s) { return s != "ok"; }
} // namespace

std::vector<const PluginRecord*> PluginDatabase::installed() const {
    return select(records, [](const PluginRecord& r) { return r.status == "ok"; });
}
std::vector<const PluginRecord*> PluginDatabase::available() const {
    return select(records, [this](const PluginRecord& r) { return r.status == "ok" && !blacklist.count(r.path); });
}
std::vector<const PluginRecord*> PluginDatabase::failed() const {
    return select(records, [](const PluginRecord& r) { return isFailure(r.status); });
}
std::vector<const PluginRecord*> PluginDatabase::blacklisted() const {
    return select(records, [this](const PluginRecord& r) { return blacklist.count(r.path) != 0; });
}
std::vector<const PluginRecord*> PluginDatabase::favoriteList() const {
    return select(records, [this](const PluginRecord& r) { return favorites.count(r.typeId) != 0; });
}
std::vector<const PluginRecord*> PluginDatabase::recentList() const {
    std::vector<const PluginRecord*> out;
    for (auto& t : recent)
        if (auto* r = find(t)) out.push_back(r);
    return out;
}
std::vector<const PluginRecord*> PluginDatabase::instruments() const {
    return select(records, [this](const PluginRecord& r) { return r.status == "ok" && r.category == "instrument" && !blacklist.count(r.path); });
}
std::vector<const PluginRecord*> PluginDatabase::effects() const {
    return select(records, [this](const PluginRecord& r) { return r.status == "ok" && r.category == "effect" && !blacklist.count(r.path); });
}
std::vector<const PluginRecord*> PluginDatabase::duplicates() const {
    return select(records, [](const PluginRecord& r) { return !r.duplicateOf.empty(); });
}

json PluginDatabase::view(const std::string& name) const {
    std::vector<const PluginRecord*> v;
    if (name == "INSTALLED") v = installed();
    else if (name == "AVAILABLE") v = available();
    else if (name == "FAILED") v = failed();
    else if (name == "BLACKLISTED") v = blacklisted();
    else if (name == "FAVORITES") v = favoriteList();
    else if (name == "RECENT") v = recentList();
    else if (name == "INSTRUMENTS") v = instruments();
    else if (name == "EFFECTS") v = effects();
    else if (name == "DUPLICATES") v = duplicates();
    json out = json::array();
    for (auto* r : v) {
        json j = r->toJson();
        j["blacklisted"] = blacklist.count(r->path) != 0;
        j["quarantined"] = quarantine.count(r->path) != 0;
        j["favorite"] = favorites.count(r->typeId) != 0;
        out.push_back(j);
    }
    return out;
}

const PluginRecord* PluginDatabase::find(const std::string& typeId) const {
    for (auto& r : records)
        if (r.typeId == typeId) return &r;
    return nullptr;
}

void PluginDatabase::setBlacklisted(const std::string& path, bool on) {
    if (on) blacklist.insert(path);
    else blacklist.erase(path);
}
void PluginDatabase::releaseQuarantine(const std::string& path) { quarantine.erase(path); }
void PluginDatabase::setFavorite(const std::string& typeId, bool on) {
    if (on) favorites.insert(typeId);
    else favorites.erase(typeId);
}
void PluginDatabase::markUsed(const std::string& typeId, size_t maxRecent) {
    recent.erase(std::remove(recent.begin(), recent.end(), typeId), recent.end());
    recent.push_front(typeId);
    while (recent.size() > maxRecent) recent.pop_back();
}
void PluginDatabase::removeRecordsForPath(const std::string& path) {
    records.erase(std::remove_if(records.begin(), records.end(), [&](const PluginRecord& r) { return r.path == path; }), records.end());
}

// ------------------------------------------------------------------ helpers
json ScanReport::toJson() const {
    return {{"modulesFound", modulesFound}, {"scanned", scanned}, {"skippedUnchanged", skippedUnchanged},
            {"skippedBlacklisted", skippedBlacklisted}, {"skippedQuarantined", skippedQuarantined}, {"pluginsOk", pluginsOk},
            {"failed", failed}, {"crashed", crashed}, {"timeouts", timeouts}, {"unsupported", unsupported},
            {"duplicates", duplicates}, {"seconds", seconds}, {"messages", messages}};
}

std::string hostArch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string binaryArch(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return "unknown";
    unsigned char h[64] = {};
    f.read(reinterpret_cast<char*>(h), sizeof(h));
    if (f.gcount() < 20) return "unknown";
    if (h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        const int machine = h[5] == 1 ? (h[18] | (h[19] << 8)) : ((h[18] << 8) | h[19]);
        switch (machine) {
        case 0x3E: return "x86_64";
        case 0x03: return "x86";
        case 0xB7: return "arm64";
        default: return "unknown";
        }
    }
    if (h[0] == 'M' && h[1] == 'Z') {
        const uint32_t off = h[0x3C] | (h[0x3D] << 8) | (h[0x3E] << 16) | (static_cast<uint32_t>(h[0x3F]) << 24);
        f.clear();
        f.seekg(off);
        unsigned char pe[6] = {};
        f.read(reinterpret_cast<char*>(pe), 6);
        if (f.gcount() == 6 && pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0) {
            const int m = pe[4] | (pe[5] << 8);
            if (m == 0x8664) return "x86_64";
            if (m == 0x14c) return "x86";
            if (m == 0xAA64) return "arm64";
        }
        return "unknown";
    }
    const uint32_t magic = h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<uint32_t>(h[3]) << 24);
    if (magic == 0xFEEDFACF) {
        const uint32_t cpu = h[4] | (h[5] << 8) | (h[6] << 16) | (static_cast<uint32_t>(h[7]) << 24);
        if (cpu == 0x01000007) return "x86_64";
        if (cpu == 0x0100000C) return "arm64";
    }
    if (magic == 0xBEBAFECA) return "universal";
    return "unknown";
}

json parseLenientJson(const std::string& text) {
    std::string s;
    s.reserve(text.size());
    bool inStr = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inStr) {
            s += c;
            if (c == '\\' && i + 1 < text.size()) s += text[++i];
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            s += c;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            ++i;
        } else if (c == ',') {
            size_t k = i + 1;
            while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
            if (k < text.size() && (text[k] == '}' || text[k] == ']')) continue; // trailing comma
            s += c;
        } else {
            s += c;
        }
    }
    return json::parse(s, nullptr, false);
}

std::vector<fs::path> defaultPluginPaths() {
    std::vector<fs::path> out;
    auto env = [](const char* n) -> std::string {
        const char* v = std::getenv(n);
        return v ? v : "";
    };
#ifdef _WIN32
    if (auto c = env("COMMONPROGRAMFILES"); !c.empty()) {
        out.push_back(fs::path(c) / "CLAP");
        out.push_back(fs::path(c) / "VST3");
    }
    if (auto l = env("LOCALAPPDATA"); !l.empty()) out.push_back(fs::path(l) / "Programs" / "Common" / "CLAP");
#elif defined(__APPLE__)
    out = {"/Library/Audio/Plug-Ins/CLAP", "/Library/Audio/Plug-Ins/VST3"};
    if (auto h = env("HOME"); !h.empty()) {
        out.push_back(fs::path(h) / "Library/Audio/Plug-Ins/CLAP");
        out.push_back(fs::path(h) / "Library/Audio/Plug-Ins/VST3");
    }
#else
    if (auto h = env("HOME"); !h.empty()) {
        out.push_back(fs::path(h) / ".clap");
        out.push_back(fs::path(h) / ".vst3");
    }
    out.push_back("/usr/lib/clap");
    out.push_back("/usr/local/lib/clap");
    out.push_back("/usr/lib/vst3");
    out.push_back("/usr/local/lib/vst3");
#endif
    if (auto extra = env("CLAP_PATH"); !extra.empty()) {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t a = 0;
        while (a <= extra.size()) {
            const size_t b = extra.find(sep, a);
            const std::string part = extra.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!part.empty()) out.push_back(part);
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    return out;
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Module {
    fs::path path;
    std::string format;
};

void collect(const fs::path& p, std::vector<Module>& out) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    const std::string ext = lower(p.extension().string());
    if (ext == ".clap") {
        out.push_back({p, "clap"}); // a file (Windows/Linux) or a bundle folder (macOS)
        return;
    }
    if (ext == ".vst3") {
        out.push_back({p, "vst3"});
        return;
    }
    if (!fs::is_directory(p, ec)) return;
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        collect(it->path(), out);
    }
}

int64_t fileTime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}
int64_t fileSize(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return 0;
    auto s = fs::file_size(p, ec);
    return ec ? 0 : static_cast<int64_t>(s);
}

// Actual binary of a module (VST3 bundles keep it under Contents/<arch>-<os>/).
fs::path moduleBinary(const Module& m) {
    std::error_code ec;
    if (!fs::is_directory(m.path, ec)) return m.path;
    const fs::path contents = m.path / "Contents";
#ifdef _WIN32
    const char* subs[] = {"x86_64-win", "arm64ec-win", "arm64-win", "x86-win"};
#elif defined(__APPLE__)
    const char* subs[] = {"MacOS"};
#else
    const char* subs[] = {"x86_64-linux", "aarch64-linux", "i386-linux"};
#endif
    for (const char* sub : subs) { // only binaries for THIS platform count
        const fs::path d = contents / sub;
        if (!fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file(ec)) return e.path();
    }
    return {};
}

void scanVst3(const Module& m, PluginRecord base, std::vector<PluginRecord>& out) {
    base.format = "vst3";
    base.status = "unsupported";
    base.error = "bundle contains no loadable binary for this platform (detected from moduleinfo.json only)";
    const fs::path info = m.path / "Contents" / "Resources" / "moduleinfo.json";
    if (auto text = files::readAll(info)) {
        json j = parseLenientJson(*text);
        if (j.is_object() && j.contains("Classes")) {
            const std::string factoryVendor = j.contains("Factory Info") ? j["Factory Info"].value("Vendor", "") : "";
            for (auto& c : j["Classes"]) {
                if (c.value("Category", "") != "Audio Module Class") continue;
                PluginRecord r = base;
                r.id = c.value("CID", "");
                r.name = c.value("Name", m.path.stem().string());
                r.vendor = c.value("Vendor", factoryVendor);
                r.version = c.value("Version", "");
                std::string sub;
                if (c.contains("Sub Categories"))
                    for (auto& s : c["Sub Categories"]) {
                        r.features.push_back(s.get<std::string>());
                        sub += s.get<std::string>() + "|";
                    }
                r.category = sub.find("Instrument") != std::string::npos ? "instrument" : (sub.find("Fx") != std::string::npos ? "effect" : "unknown");
                r.typeId = "vst3:" + m.path.string() + "|" + (r.id.empty() ? r.name : r.id);
                out.push_back(r);
            }
            if (!out.empty()) return;
        }
    }
    base.name = m.path.stem().string();
    base.category = "unknown";
    base.typeId = "vst3:" + m.path.string() + "|" + base.name;
    out.push_back(base);
}
} // namespace

ScanReport scanPlugins(PluginDatabase& db, const ScanOptions& options) {
    const auto t0 = std::chrono::steady_clock::now();
    ScanReport rep;
    std::vector<Module> modules;
    for (auto& p : options.paths) collect(p, modules);
    std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.path < b.path; });
    modules.erase(std::unique(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.path == b.path; }), modules.end());
    rep.modulesFound = static_cast<int>(modules.size());
    const std::string hostExe = options.hostExe.empty() ? hostExecutable() : options.hostExe;
    const std::string myArch = hostArch();

    for (auto& m : modules) {
        const std::string path = m.path.string();
        if (db.isBlacklisted(path)) {
            ++rep.skippedBlacklisted;
            continue;
        }
        if (db.quarantine.count(path) && !options.retryQuarantined) {
            ++rep.skippedQuarantined;
            rep.messages.push_back("quarantined (skipped): " + path);
            continue;
        }
        const int64_t size = fileSize(m.path), mtime = fileTime(m.path);
        if (!options.force) {
            bool unchanged = false;
            for (auto& r : db.records)
                if (r.path == path) {
                    unchanged = r.fileSize == size && r.fileTime == mtime && (r.status == "ok" || r.status == "unsupported");
                    break;
                }
            if (unchanged) {
                ++rep.skippedUnchanged;
                continue;
            }
        }
        db.removeRecordsForPath(path);
        db.quarantine.erase(path);
        ++rep.scanned;

        PluginRecord base;
        base.format = m.format;
        base.path = path;
        base.fileSize = size;
        base.fileTime = mtime;
        base.scannedAt = files::nowIso8601();
        const fs::path bin = moduleBinary(m);
        base.arch = bin.empty() ? "unknown" : binaryArch(bin);
        std::vector<PluginRecord> found;
        const auto s0 = std::chrono::steady_clock::now();

        if (m.format == "vst3" && bin.empty()) {
            scanVst3(m, base, found);
        } else if (base.arch != "unknown" && base.arch != "universal" && base.arch != myArch) {
            base.status = "wrong_arch";
            base.error = std::format("module is {} but RoY Studio runs as {}", base.arch, myArch);
            base.typeId = "file:" + path;
            base.name = m.path.stem().string();
            found.push_back(base);
        } else {
            ChildProcess child;
            std::string err;
            base.typeId = "file:" + path;
            base.name = m.path.stem().string();
            if (!child.start(hostExe, {"--scan", path}, &err)) {
                base.status = "failed";
                base.error = "cannot start plugin host: " + err;
                found.push_back(base);
            } else {
                std::string line;
                const bool got = child.readLine(line, options.timeoutMs);
                if (!got) {
                    const bool exited = child.wait(100);
                    if (exited && child.crashed()) {
                        base.status = "crashed";
                        base.error = "plugin crashed the scanner: " + child.terminationReason();
                    } else if (exited) {
                        // RoY's scanner always answers before it exits: ending without a result
                        // means the plugin terminated the process (exit()/abort handler) - quarantine it.
                        base.status = "crashed";
                        base.error = std::format("plugin crashed or terminated the scanner without a result (exit code {})", child.exitCode());
                    } else {
                        child.kill();
                        base.status = "timeout";
                        base.error = std::format("no answer within {} ms - scanner terminated", options.timeoutMs);
                    }
                    found.push_back(base);
                } else {
                    child.wait(2000);
                    json r = json::parse(line, nullptr, false);
                    if (r.is_discarded() || !r.value("ok", false)) {
                        base.status = "failed";
                        base.error = r.is_object() ? r.value("error", std::string("scan failed")) : "unreadable scanner output";
                        found.push_back(base);
                    } else {
                        for (auto& p : r["plugins"]) {
                            PluginRecord rec = base;
                            rec.id = p.value("id", "");
                            rec.name = p.value("name", rec.id);
                            rec.vendor = p.value("vendor", "");
                            rec.version = p.value("version", "");
                            for (auto& f : p.value("features", json::array())) rec.features.push_back(f.get<std::string>());
                            rec.category = p.value("instrument", false) ? "instrument" : "effect";
                            rec.paramCount = p.value("paramCount", 0);
                            rec.typeId = m.format == "vst3" ? makeVst3TypeId(path, rec.id) : makeClapTypeId(path, rec.id);
                            rec.status = p.value("instantiates", false) ? "ok" : "failed";
                            rec.error = p.value("error", "");
                            found.push_back(rec);
                        }
                        if (found.empty()) {
                            base.status = "failed";
                            base.error = "module contains no plugins";
                            found.push_back(base);
                        }
                    }
                }
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        for (auto& r : found) {
            r.scanMs = ms;
            if (r.status == "ok") ++rep.pluginsOk;
            else if (r.status == "crashed") ++rep.crashed;
            else if (r.status == "timeout") ++rep.timeouts;
            else if (r.status == "unsupported") ++rep.unsupported;
            else ++rep.failed;
            if (r.status == "crashed" || r.status == "timeout") {
                db.quarantine.insert(path);
                log::warn("plugins", "QUARANTINED {}: {}", path, r.error);
                rep.messages.push_back(std::format("quarantined: {} ({})", path, r.error));
            }
            db.records.push_back(r);
        }
    }

    // Duplicates: the same plugin id (or name+vendor across formats) in more than one place.
    std::map<std::string, std::string> firstById;
    for (auto& r : db.records) {
        r.duplicateOf.clear();
        if (r.status != "ok" && r.status != "unsupported") continue;
        const std::string key = r.id.empty() ? lower(r.vendor + "|" + r.name) : r.id;
        auto [it, inserted] = firstById.emplace(key, r.typeId);
        if (!inserted) {
            r.duplicateOf = it->second;
            ++rep.duplicates;
        }
    }
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log::info("plugins", "scan: {} modules, {} scanned, {} ok, {} failed, {} crashed, {} timeouts, {} unsupported, {:.2f} s",
              rep.modulesFound, rep.scanned, rep.pluginsOk, rep.failed, rep.crashed, rep.timeouts, rep.unsupported, rep.seconds);
    return rep;
}

} // namespace roy::plugins
