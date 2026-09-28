#include "project/Session.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Process.h"

#include <format>

namespace roy {

namespace fs = std::filesystem;

namespace {
fs::path lockPath(const fs::path& projectFile) { return projectFile.parent_path() / ".roy_session.lock"; }
fs::path snapshotPath(const fs::path& projectFile) { return projectFile.parent_path() / "RECOVERY" / "autosave.roy"; }

bool readable(const fs::path& f) {
    Project p;
    auto text = files::readAll(f);
    return text && deserializeProject(*text, p);
}
} // namespace

ProjectSession::~ProjectSession() {
    if (isOpen()) removeLock(); // destructor = best effort clean close
}

RecoveryInfo ProjectSession::inspect(const fs::path& projectFile) {
    RecoveryInfo info;
    std::error_code ec;
    if (auto lock = files::readAll(lockPath(projectFile))) {
        info.lockFound = true;
        json j = json::parse(*lock, nullptr, false);
        if (!j.is_discarded()) {
            info.lockPid = j.value("pid", 0);
            info.lockHost = j.value("host", "");
        }
        info.lockOwnerAlive = info.lockHost == hostName() && info.lockPid != currentProcessId() && isProcessAlive(info.lockPid);
        info.crashed = !info.lockOwnerAlive;
    }
    const fs::path snap = snapshotPath(projectFile);
    if (fs::exists(snap, ec)) {
        info.snapshotAvailable = true;
        info.snapshot = snap;
        const auto st = fs::last_write_time(snap, ec);
        const auto pt = fs::last_write_time(projectFile, ec);
        info.snapshotNewer = ec ? true : st > pt;
        if (auto text = files::readAll(snap)) {
            json j = json::parse(*text, nullptr, false);
            if (!j.is_discarded()) info.snapshotTime = j.value("modifiedAt", "");
        }
    }
    info.projectFileReadable = readable(projectFile);
    auto backups = listBackups(projectFile);
    for (auto it = backups.rbegin(); it != backups.rend(); ++it)
        if (readable(*it)) {
            info.lastGoodBackup = *it;
            break;
        }
    return info;
}

bool ProjectSession::writeLock(std::string* error) {
    json j = {{"pid", currentProcessId()}, {"host", hostName()}, {"openedAt", files::nowIso8601()}};
    return files::atomicWrite(lockPath(file_), j.dump(), error);
}

void ProjectSession::removeLock() {
    std::error_code ec;
    fs::remove(lockPath(file_), ec);
}

bool ProjectSession::create(const fs::path& parentDir, Project& project, std::string* error) {
    const fs::path f = createProjectFolder(parentDir, project.name);
    SaveOptions opt;
    opt.makeBackup = false;
    auto r = saveProject(project, f, opt);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    file_ = f;
    dirty_ = false;
    fromRecovery_ = false;
    ownsSnapshot_ = false;
    lastAutosave_ = std::chrono::steady_clock::now();
    return writeLock(error);
}

bool ProjectSession::open(const fs::path& projectFile, Project& project, OpenMode mode, std::string* error) {
    const RecoveryInfo info = inspect(projectFile);
    if (info.lockOwnerAlive) {
        if (error) *error = std::format("project is already open in another RoY Studio instance (pid {})", info.lockPid);
        return false;
    }
    fromRecovery_ = false;
    fs::path source = projectFile;
    switch (mode) {
    case OpenMode::Normal: break;
    case OpenMode::RecoverProject:
        if (!info.snapshotAvailable) {
            if (error) *error = "no recovery snapshot available";
            return false;
        }
        source = info.snapshot;
        fromRecovery_ = true;
        break;
    case OpenMode::OpenLastStable:
        if (!info.projectFileReadable) {
            if (info.lastGoodBackup.empty()) {
                if (error) *error = "project file is unreadable and no readable backup exists";
                return false;
            }
            source = info.lastGoodBackup;
            log::warn("session", "project file unreadable, opening last good backup {}", source.string());
        }
        break;
    case OpenMode::DiscardRecovery:
        if (info.snapshotAvailable) {
            std::error_code ec;
            const fs::path aside = files::uniquePath(projectFile.parent_path() / "RECOVERY" /
                                                     std::format("discarded_{}.roy", files::nowCompact()));
            fs::rename(info.snapshot, aside, ec);
            if (ec) {
                if (error) *error = "could not move recovery snapshot aside: " + ec.message();
                return false;
            }
        }
        break;
    }
    auto r = loadProject(source, project);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    file_ = projectFile;
    newerFormat_ = r.newerThanSupported;
    dirty_ = fromRecovery_ || r.migrated; // recovered state is unsaved until the user saves
    // A snapshot left by a crash belongs to the user's decision (recover or
    // discard) - this session must not rotate it away unless it recovered it.
    ownsSnapshot_ = fromRecovery_;
    lastAutosave_ = std::chrono::steady_clock::now();
    return writeLock(error);
}

bool ProjectSession::save(const Project& project, std::string* error) {
    if (!isOpen()) {
        if (error) *error = "no project open";
        return false;
    }
    SaveOptions opt;
    opt.backupRetention = backupRetention;
    auto r = saveProject(project, file_, opt);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    dirty_ = false;
    fromRecovery_ = false;
    // The autosave is superseded; keep one previous copy rather than deleting.
    std::error_code ec;
    const fs::path snap = snapshotPath(file_);
    if (ownsSnapshot_ && fs::exists(snap, ec)) {
        const fs::path prev = snap.parent_path() / "autosave_prev.roy";
        fs::remove(prev, ec);
        fs::rename(snap, prev, ec);
    }
    return true;
}

bool ProjectSession::saveAs(const Project& project, const fs::path& newFile, std::string* error) {
    std::error_code ec;
    if (fs::exists(newFile, ec)) {
        if (error) *error = "file exists: " + newFile.string();
        return false;
    }
    for (const char* sub : {"Audio", "Exports", "BACKUPS", "RECOVERY", "Stems"}) fs::create_directories(newFile.parent_path() / sub, ec);
    SaveOptions opt;
    opt.makeBackup = false;
    auto r = saveProject(project, newFile, opt);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    if (isOpen()) removeLock();
    file_ = newFile;
    dirty_ = false;
    newerFormat_ = false;
    return writeLock(error);
}

bool ProjectSession::autosave(const Project& project, bool force) {
    if (!isOpen()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!force && (!dirty_ || now - lastAutosave_ < std::chrono::seconds(autosaveIntervalSec))) return false;
    Project p = project;
    p.modifiedAt = files::nowIso8601();
    std::string err;
    if (!files::atomicWrite(snapshotPath(file_), serializeProject(p), &err)) {
        log::error("session", "autosave failed: {}", err);
        return false;
    }
    lastAutosave_ = now;
    ownsSnapshot_ = true;
    log::debug("session", "autosaved {}", snapshotPath(file_).string());
    return true;
}

fs::path ProjectSession::milestone(const fs::path& snapshotsDir, const std::string& label, std::string* error) const {
    if (!isOpen()) return {};
    const fs::path target = files::uniquePath(snapshotsDir / std::format("{}_{}_{}.roy", file_.stem().string(),
                                                                         sanitizeFileName(label), files::nowCompact()));
    if (!files::safeCopy(file_, target, error)) return {};
    return target;
}

void ProjectSession::close() {
    if (!isOpen()) return;
    std::error_code ec;
    const fs::path snap = snapshotPath(file_);
    if (!dirty_ && ownsSnapshot_ && fs::exists(snap, ec)) {
        const fs::path prev = snap.parent_path() / "autosave_prev.roy";
        fs::remove(prev, ec);
        fs::rename(snap, prev, ec);
    }
    removeLock();
    file_.clear();
    dirty_ = false;
    ownsSnapshot_ = false;
}

} // namespace roy
