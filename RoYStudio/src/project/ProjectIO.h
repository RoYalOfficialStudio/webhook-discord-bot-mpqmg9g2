#pragma once
// Project file format (JSON, versioned) and safe save/load.
//
// Project folder layout:
//   <Name>/<Name>.roy        project file (JSON)
//   <Name>/Audio/            recordings and imported audio (never overwritten)
//   <Name>/Exports/          renders
//   <Name>/BACKUPS/          <Name>_0001.roy, <Name>_0002.roy, ...
//   <Name>/RECOVERY/         autosave snapshots for crash recovery
//   <Name>/.roy_session.lock present while the project is open
#include "project/Project.h"

#include <filesystem>
#include <optional>
#include <string>

namespace roy {

json projectToJson(const Project& p);
// Parses (and migrates) a project. Unknown keys are preserved in p.unknown.
bool projectFromJson(const json& j, Project& p, std::string* error = nullptr, std::vector<std::string>* migrationLog = nullptr);

std::string serializeProject(const Project& p, bool pretty = true);
bool deserializeProject(const std::string& text, Project& p, std::string* error = nullptr);

// Upgrades an older document in place. Returns false if it cannot be migrated.
bool migrateProjectJson(json& j, std::vector<std::string>* log = nullptr);

struct LoadResult {
    bool ok = false;
    std::string error;
    int fileVersion = 0;
    bool migrated = false;
    bool newerThanSupported = false; // opened a file from a newer RoY Studio: saving over it is refused
    std::vector<std::string> migrationLog;
    std::filesystem::path migrationBackup; // copy of the original file made before migration
};

struct SaveOptions {
    bool makeBackup = true;   // copy the previous file into BACKUPS/ before replacing it
    int backupRetention = 20; // number of numbered backups kept (0 = unlimited)
    bool allowOverwriteNewer = false;
};

struct SaveResult {
    bool ok = false;
    std::string error;
    std::filesystem::path backupPath;
};

LoadResult loadProject(const std::filesystem::path& file, Project& p);
SaveResult saveProject(const Project& p, const std::filesystem::path& file, const SaveOptions& opt = {});

// Creates <parentDir>/<name>/ with the standard sub folders; returns the .roy path.
std::filesystem::path createProjectFolder(const std::filesystem::path& parentDir, const std::string& name);
std::filesystem::path projectFolderOf(const std::filesystem::path& projectFile);
std::string sanitizeFileName(const std::string& name);

// Numbered backups in <projectFolder>/BACKUPS.
std::vector<std::filesystem::path> listBackups(const std::filesystem::path& projectFile);
std::filesystem::path writeBackup(const std::filesystem::path& projectFile, int retention, std::string* error = nullptr);

} // namespace roy
