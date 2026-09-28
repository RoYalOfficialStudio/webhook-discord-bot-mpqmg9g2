#include "commands/UndoManager.h"
#include "core/Log.h"
#include "project/ProjectIO.h"

namespace roy {

namespace {
std::string snapshot(const Project& p) {
    // modifiedAt changes on every save; exclude it so no-op edits are detected.
    json j = projectToJson(p);
    j.erase("modifiedAt");
    return j.dump();
}
} // namespace

void UndoManager::begin(const std::string& name) {
    if (depth_++ == 0) {
        pendingName_ = name;
        before_ = snapshot(project_);
    }
}

bool UndoManager::end() {
    if (depth_ == 0) return false;
    if (--depth_ > 0) return false;
    std::string after = snapshot(project_);
    if (after == before_) {
        before_.clear();
        return false;
    }
    undo_.push_back({pendingName_, std::move(before_), std::move(after)});
    redo_.clear();
    while (undo_.size() > limit_) undo_.pop_front();
    before_.clear();
    return true;
}

void UndoManager::cancel() {
    if (depth_ == 0) return;
    depth_ = 0;
    restore(before_);
    before_.clear();
}

void UndoManager::restore(const std::string& state) {
    Project p;
    std::string err;
    if (!deserializeProject(state, p, &err)) {
        log::error("undo", "cannot restore state: {}", err);
        return;
    }
    p.modifiedAt = project_.modifiedAt;
    project_ = std::move(p);
    if (onRestore) onRestore(project_);
}

bool UndoManager::undo() {
    if (depth_ > 0 || undo_.empty()) return false;
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    restore(e.before);
    redo_.push_back(std::move(e));
    return true;
}

bool UndoManager::redo() {
    if (depth_ > 0 || redo_.empty()) return false;
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    restore(e.after);
    undo_.push_back(std::move(e));
    return true;
}

void UndoManager::clear() {
    undo_.clear();
    redo_.clear();
}

size_t UndoManager::memoryBytes() const {
    size_t n = 0;
    for (auto& e : undo_) n += e.before.size() + e.after.size();
    for (auto& e : redo_) n += e.before.size() + e.after.size();
    return n;
}

} // namespace roy
