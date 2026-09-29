#pragma once
// CHANNEL PRESETS ("vocal chains"): a mixer channel's effect chain (types, names, bypass,
// complete effect/plugin state) plus fader, pan and width, stored as a small JSON file so a
// mixed vocal can be reused in the next song. Sends and sidechains are project-specific and are
// not stored. Files live in <user data>/Presets/Channel/<name>.roychain (portable build: inside
// the RoY Studio folder).
#include "project/Project.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace roy::presets {

namespace fs = std::filesystem;

inline constexpr const char* kChannelPresetFormat = "roy.channelPreset";
inline constexpr const char* kChannelPresetExt = ".roychain";

// Snapshot of a channel (call ProjectRuntime::captureProcessorStates first so plugin states are current).
json makeChannelPreset(const MixerChannel& ch, const std::string& name);
// Checks format, version and the insert list. Returns false with a reason.
bool validChannelPreset(const json& preset, std::string* error = nullptr);

fs::path channelPresetDirectory(); // <user data>/Presets/Channel (created on demand)
// Safe file name for a preset name ("Rap Vocal / hard" -> "Rap Vocal _ hard").
std::string presetFileStem(const std::string& name);

struct PresetFile {
    std::string name;
    fs::path file;
    bool factory = false; // built into RoY (not a file)
};
// Saves `preset` as <dir>/<stem>.roychain. An existing preset of the same name is only replaced
// with overwrite = true, and its old version is first copied to <dir>/Backups (never lost).
bool saveChannelPreset(const fs::path& dir, const json& preset, bool overwrite, fs::path* written, std::string* error);
std::optional<json> loadChannelPreset(const fs::path& file, std::string* error = nullptr);
// User presets in `dir`, sorted by name (unreadable files are skipped).
std::vector<PresetFile> listChannelPresets(const fs::path& dir);
// Built-in starting points made only of RoY effects (free, always available).
std::vector<json> factoryChannelPresets();

} // namespace roy::presets
