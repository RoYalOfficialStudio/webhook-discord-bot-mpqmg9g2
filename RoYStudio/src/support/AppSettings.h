#pragma once
// APP SETTINGS (per user, not per project): audio device + buffer + sample rate, MIDI inputs the
// user switched off, first-run state. Stored as <userData>/settings.json, written atomically.
// A damaged file is kept as settings.json.corrupt-N and defaults are used (never a crash, never
// silently overwritten without a copy). Unknown keys from newer versions are preserved.
#include "audio/DeviceManager.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace roy::support {

struct AppSettings {
    AudioDeviceConfig audio;
    std::vector<std::string> midiInputsOff; // ids the user switched off (not reopened by hot-plug)
    bool firstRunDone = false;
    nlohmann::json unknown = nlohmann::json::object();
};

nlohmann::json settingsToJson(const AppSettings& s);
AppSettings settingsFromJson(const nlohmann::json& j); // tolerant: bad values -> defaults

// Returns defaults when the file does not exist. `note` explains a recovered damaged file.
AppSettings loadSettings(const std::filesystem::path& file, std::string* note = nullptr);
bool saveSettings(const std::filesystem::path& file, const AppSettings& s, std::string* error = nullptr);

} // namespace roy::support
