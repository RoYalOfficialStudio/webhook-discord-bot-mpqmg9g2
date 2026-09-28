#pragma once
// Central command system. Every important UI action is a named command with
// JSON arguments, so the same action is available to menus, keyboard
// shortcuts, the command palette, macros, scripting and tests - and every
// undoable command automatically becomes one undo step.
#include "commands/UndoManager.h"
#include "project/Project.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace roy {

class ProjectRuntime;

struct CommandContext {
    Project& project;
    UndoManager& undo;
    ProjectRuntime* runtime = nullptr;          // optional: audio-side operations (normalize, analysis)
    std::filesystem::path projectFolder;        // where new audio files (processed vocals, renders) are written
    std::function<void(bool structural)> changed; // notify: structural -> rebuild graph, else sync params
    json result = json::object();               // command output (e.g. created ids)
    std::string error;
};

using CommandFn = std::function<bool(CommandContext&, const json& args)>;

struct CommandInfo {
    std::string id;       // "AddTrack"
    std::string title;    // "Add Track"
    std::string category; // "Track", "Clip", "Mixer", ...
    std::string shortcut; // "Ctrl+T"
    bool undoable = true;
    bool structural = true; // requires an audio graph rebuild
    CommandFn fn;
};

class CommandRegistry {
public:
    void add(CommandInfo info);
    const CommandInfo* find(const std::string& id) const;
    std::vector<const CommandInfo*> list(const std::string& category = {}) const;
    // Fuzzy search for the command palette (matches id/title, case-insensitive subsequence).
    std::vector<const CommandInfo*> search(const std::string& query) const;
    const CommandInfo* byShortcut(const std::string& shortcut) const;
    bool setShortcut(const std::string& id, const std::string& shortcut);

    // Runs a command inside an undo transaction. On failure the transaction is
    // cancelled and the project is left unchanged.
    bool execute(CommandContext& ctx, const std::string& id, const json& args = json::object()) const;
    // Runs several commands as ONE undo step (macro). All-or-nothing.
    bool executeMacro(CommandContext& ctx, const std::string& name,
                      const std::vector<std::pair<std::string, json>>& steps) const;

private:
    std::map<std::string, CommandInfo> commands_;
};

// Registers the built-in command set (tracks, clips, mixer, automation, markers, MIDI, ...).
void registerCoreCommands(CommandRegistry& r);
// Vocal Lab commands (Pitch Guardian, Double Magnet, Vocal Doctor, region edits). Called by registerCoreCommands.
void registerVocalCommands(CommandRegistry& r);
// Import, sampler slicing, sample analysis, stem separation. Called by registerCoreCommands.
void registerProductionCommands(CommandRegistry& r);
// Master chain presets and export. Called by registerCoreCommands.
void registerMasterCommands(CommandRegistry& r);
// Mix analysis, energy map, sections, project assistant, Vocal DNA. Called by registerCoreCommands.
void registerIntelligenceCommands(CommandRegistry& r);
// Plugin status / crash recovery (sandboxed CLAP plugins). Called by registerCoreCommands.
void registerPluginCommands(CommandRegistry& r);
// Beat Lab: patterns, steps, swing, pattern clips. Called by registerCoreCommands.
void registerBeatCommands(CommandRegistry& r);

} // namespace roy
