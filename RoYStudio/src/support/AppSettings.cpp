#include "support/AppSettings.h"
#include "core/Files.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace roy::support {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
template <typename T>
T get(const json& j, const char* key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}
} // namespace

json settingsToJson(const AppSettings& s) {
    json j = s.unknown.is_object() ? s.unknown : json::object();
    j["version"] = 1;
    j["audio"] = {{"backend", s.audio.backend},       {"outputDevice", s.audio.outputDevice}, {"inputDevice", s.audio.inputDevice},
                  {"sampleRate", s.audio.sampleRate}, {"bufferSize", s.audio.bufferSize},     {"enableInput", s.audio.enableInput},
                  {"exclusive", s.audio.exclusive}};
    j["midi"] = {{"inputsOff", s.midiInputsOff}};
    j["firstRunDone"] = s.firstRunDone;
    return j;
}

AppSettings settingsFromJson(const json& j) {
    AppSettings s;
    if (!j.is_object()) return s;
    for (auto& [k, v] : j.items())
        if (k != "version" && k != "audio" && k != "midi" && k != "firstRunDone") s.unknown[k] = v;
    const json a = j.value("audio", json::object());
    if (a.is_object()) {
        static const char* backends[] = {"auto", "wasapi", "dsound", "winmm", "alsa", "pulseaudio", "jack", "coreaudio", "null"};
        const std::string b = get<std::string>(a, "backend", "auto");
        if (std::find(std::begin(backends), std::end(backends), b) != std::end(backends)) s.audio.backend = b;
        s.audio.outputDevice = get<std::string>(a, "outputDevice", "");
        s.audio.inputDevice = get<std::string>(a, "inputDevice", "");
        const double sr = get<double>(a, "sampleRate", 48000.0);
        if (std::isfinite(sr) && sr >= 8000.0 && sr <= 384000.0) s.audio.sampleRate = sr;
        const int bs = get<int>(a, "bufferSize", 256);
        if (bs >= 16 && bs <= 8192) s.audio.bufferSize = bs;
        s.audio.enableInput = get<bool>(a, "enableInput", true);
        s.audio.exclusive = get<bool>(a, "exclusive", false);
    }
    const json m = j.value("midi", json::object());
    if (m.is_object() && m.contains("inputsOff") && m["inputsOff"].is_array())
        for (auto& id : m["inputsOff"])
            if (id.is_string()) s.midiInputsOff.push_back(id.get<std::string>());
    s.firstRunDone = get<bool>(j, "firstRunDone", false);
    return s;
}

AppSettings loadSettings(const fs::path& file, std::string* note) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return {};
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        const json j = json::parse(ss.str());
        if (!j.is_object()) throw std::runtime_error("not an object");
        return settingsFromJson(j);
    } catch (const std::exception& e) {
        const fs::path keep = files::uniquePath(fs::path(file.string() + ".corrupt"));
        fs::copy_file(file, keep, ec);
        if (note) *note = "settings file was damaged (" + std::string(e.what()) + ") - defaults used, the old file is kept as " + keep.filename().string();
        return {};
    }
}

bool saveSettings(const fs::path& file, const AppSettings& s, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    return files::atomicWrite(file, settingsToJson(s).dump(2), error);
}

} // namespace roy::support
