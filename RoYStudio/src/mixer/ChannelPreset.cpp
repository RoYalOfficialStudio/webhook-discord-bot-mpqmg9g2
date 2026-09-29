#include "mixer/ChannelPreset.h"
#include "core/Files.h"

#include <algorithm>
#include <cctype>

namespace roy::presets {

json makeChannelPreset(const MixerChannel& ch, const std::string& name) {
    json inserts = json::array();
    for (auto& s : ch.inserts)
        inserts.push_back({{"typeId", s.typeId}, {"name", s.name}, {"bypass", s.bypass}, {"state", s.state}});
    return {{"format", kChannelPresetFormat},
            {"version", 1},
            {"name", name},
            {"created", files::nowIso8601()},
            {"app", ROY_VERSION_STRING},
            {"gainDb", ch.gainDb},
            {"pan", ch.pan},
            {"width", ch.width},
            {"inserts", inserts}};
}

bool validChannelPreset(const json& p, std::string* error) {
    auto bad = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    if (!p.is_object() || p.value("format", std::string()) != kChannelPresetFormat) return bad("not a RoY channel preset");
    if (!p.contains("version") || !p["version"].is_number_integer() || p["version"].get<int>() < 1) return bad("preset version missing");
    if (p["version"].get<int>() > 1) return bad("preset was made by a newer RoY Studio");
    if (!p.contains("inserts") || !p["inserts"].is_array()) return bad("preset has no effect list");
    if (p["inserts"].size() > 32) return bad("preset has more than 32 effects");
    for (auto& i : p["inserts"])
        if (!i.is_object() || !i.contains("typeId") || !i["typeId"].is_string() || i["typeId"].get<std::string>().empty())
            return bad("preset contains an invalid effect entry");
    for (const char* k : {"gainDb", "pan", "width"})
        if (p.contains(k) && !p[k].is_number()) return bad(std::string("preset value '") + k + "' is not a number");
    return true;
}

fs::path channelPresetDirectory() {
    const fs::path d = files::userDataDirectory() / "Presets" / "Channel";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string presetFileStem(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (c >= 0x80 || std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '+' || c == '.') s += static_cast<char>(c);
        else s += '_';
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back(); // Windows strips these
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    if (s.size() > 60) s.resize(60);
    if (s.empty()) s = "Preset";
    std::string upper = s;
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"};
    for (auto r : reserved)
        if (upper == r) return "_" + s;
    return s;
}

bool saveChannelPreset(const fs::path& dir, const json& preset, bool overwrite, fs::path* written, std::string* error) {
    if (!validChannelPreset(preset, error)) return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (presetFileStem(preset.value("name", std::string("Preset"))) + kChannelPresetExt);
    if (fs::exists(file, ec)) {
        if (!overwrite) {
            if (error) *error = "a preset with this name already exists";
            return false;
        }
        // keep the previous version: copy it to Backups first
        const fs::path backup = files::uniquePath(dir / "Backups" / (file.stem().string() + "_" + files::nowCompact() + kChannelPresetExt));
        fs::create_directories(backup.parent_path(), ec);
        if (!files::safeCopy(file, backup, error)) return false;
    }
    if (!files::atomicWrite(file, preset.dump(2), error)) return false;
    if (written) *written = file;
    return true;
}

std::optional<json> loadChannelPreset(const fs::path& file, std::string* error) {
    const auto text = files::readAll(file);
    if (!text) {
        if (error) *error = "cannot read " + file.filename().string();
        return std::nullopt;
    }
    json p = json::parse(*text, nullptr, false);
    if (p.is_discarded()) {
        if (error) *error = file.filename().string() + " is damaged (not valid JSON)";
        return std::nullopt;
    }
    if (!validChannelPreset(p, error)) return std::nullopt;
    return p;
}

std::vector<PresetFile> listChannelPresets(const fs::path& dir) {
    std::vector<PresetFile> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != kChannelPresetExt) continue;
        if (auto p = loadChannelPreset(it->path())) out.push_back({p->value("name", it->path().stem().string()), it->path(), false});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

std::vector<json> factoryChannelPresets() {
    auto fx = [](const char* type, const char* name, json params) {
        return json{{"typeId", type}, {"name", name}, {"bypass", false}, {"state", {{"params", std::move(params)}}}};
    };
    auto preset = [](const char* name, json inserts) {
        return json{{"format", kChannelPresetFormat}, {"version", 1}, {"name", name}, {"factory", true}, {"inserts", std::move(inserts)}};
    };
    const json eq = {{"lowCut", 90.0}, {"band2Freq", 300.0}, {"band2Gain", -2.0}, {"band3Freq", 3500.0}, {"band3Gain", 2.0}, {"band4Gain", 2.5}};
    const json deess = {{"frequency", 6500.0}, {"threshold", -30.0}, {"range", 8.0}};
    return {
        preset("RoY Rap Vocal - Autotune hard",
               {fx("roy.vocaltune", "RoY VocalTune", {{"speed", 5.0}, {"strength", 1.0}, {"threshold", 5.0}}),
                fx("roy.eq", "RoY EQ", eq),
                fx("roy.compressor", "RoY Compressor", {{"threshold", -22.0}, {"ratio", 4.0}, {"attack", 5.0}, {"release", 80.0}, {"makeup", 4.0}}),
                fx("roy.deesser", "RoY De-Esser", deess),
                fx("roy.reverb", "RoY Reverb", {{"decay", 1.2}, {"preDelay", 30.0}, {"mix", 0.12}})}),
        preset("RoY Vocal - Natural tune",
               {fx("roy.vocaltune", "RoY VocalTune", {{"speed", 60.0}, {"strength", 0.7}}),
                fx("roy.eq", "RoY EQ", eq),
                fx("roy.compressor", "RoY Compressor", {{"threshold", -18.0}, {"ratio", 3.0}, {"attack", 10.0}, {"release", 120.0}, {"makeup", 3.0}}),
                fx("roy.deesser", "RoY De-Esser", deess),
                fx("roy.reverb", "RoY Reverb", {{"decay", 1.8}, {"preDelay", 25.0}, {"mix", 0.16}})}),
        preset("RoY Vocal - Clean (no tune)",
               {fx("roy.eq", "RoY EQ", eq),
                fx("roy.compressor", "RoY Compressor", {{"threshold", -18.0}, {"ratio", 3.0}, {"attack", 10.0}, {"release", 120.0}, {"makeup", 3.0}}),
                fx("roy.deesser", "RoY De-Esser", deess)}),
    };
}

} // namespace roy::presets
