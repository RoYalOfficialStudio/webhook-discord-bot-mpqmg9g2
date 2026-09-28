#pragma once
// An open project on disk: session lock, autosave, crash recovery, backups
// and milestone snapshots.
//
// Crash recovery rules:
//  * While a project is open, <folder>/.roy_session.lock exists.
//  * Autosaves go to <folder>/RECOVERY/autosave.roy - NEVER over the project file.
//  * On open, a leftover lock from a dead process + a newer autosave means a
//    crash happened: the caller offers RECOVER PROJECT / OPEN LAST STABLE /
//    DISCARD RECOVERY. Discarding moves the snapshot aside, it never deletes it.
#include "project/ProjectIO.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace roy {

struct RecoveryInfo {
    bool lockFound = false;
    bool lockOwnerAlive = false;     // another RoY instance still has it open
    int lockPid = 0;
    std::string lockHost;
    bool crashed = false;            // stale lock (owner dead)
    bool snapshotAvailable = false;  // RECOVERY/autosave.roy exists
    bool snapshotNewer = false;      // snapshot is newer than the project file
    std::filesystem::path snapshot;
    std::string snapshotTime;
    bool projectFileReadable = true; // false if the main file is corrupt/unreadable
    std::filesystem::path lastGoodBackup;
};

enum class OpenMode {
    Normal,          // open the project file
    RecoverProject,  // load the recovery snapshot (project file untouched)
    OpenLastStable,  // load the project file, or the newest readable backup if it is corrupt
    DiscardRecovery  // move the recovery snapshot aside, then open normally
};

class ProjectSession {
public:
    ProjectSession() = default;
    ~ProjectSession();
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    static RecoveryInfo inspect(const std::filesystem::path& projectFile);

    // Creates a new project folder and writes the initial file.
    bool create(const std::filesystem::path& parentDir, Project& project, std::string* error = nullptr);
    bool open(const std::filesystem::path& projectFile, Project& project, OpenMode mode = OpenMode::Normal,
              std::string* error = nullptr);
    // Saves to the project file (with numbered backup). Clears the dirty flag.
    bool save(const Project& project, std::string* error = nullptr);
    // Saves under a new file name/location (never overwrites an existing file).
    bool saveAs(const Project& project, const std::filesystem::path& newFile, std::string* error = nullptr);
    // Clean close: removes the lock. Autosave snapshot is archived.
    void close();

    void markDirty() { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    // Writes RECOVERY/autosave.roy if dirty and the interval elapsed (or force).
    bool autosave(const Project& project, bool force = false);
    std::chrono::steady_clock::time_point lastAutosave() const { return lastAutosave_; }

    // Copies the current project file into `snapshotsDir` with a label.
    std::filesystem::path milestone(const std::filesystem::path& snapshotsDir, const std::string& label,
                                    std::string* error = nullptr) const;

    bool isOpen() const { return !file_.empty(); }
    const std::filesystem::path& file() const { return file_; }
    std::filesystem::path folder() const { return file_.parent_path(); }
    std::filesystem::path audioFolder() const { return folder() / "Audio"; }
    std::filesystem::path exportFolder() const { return folder() / "Exports"; }
    std::filesystem::path recoveryFolder() const { return folder() / "RECOVERY"; }
    bool openedFromRecovery() const { return fromRecovery_; }
    bool readOnlyNewerFormat() const { return newerFormat_; }
    int backupRetention = 20;
    int autosaveIntervalSec = 120;

private:
    bool writeLock(std::string* error);
    void removeLock();

    std::filesystem::path file_;
    bool dirty_ = false;
    bool fromRecovery_ = false;
    bool newerFormat_ = false;
    bool ownsSnapshot_ = false; // the RECOVERY snapshot was written by (or recovered into) this session
    std::chrono::steady_clock::time_point lastAutosave_{};
};

} // namespace roy
