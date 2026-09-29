#pragma once
// Undo/redo using project mementos (compact JSON of the whole model).
// Audio files are never touched by undo/redo - only the model referencing them.
// Transactions nest: only the outermost begin()/end() records an entry, so a
// macro or a compound edit is undone in one step.
// Memory: consecutive steps share their common state (step N's "after" is step N+1's
// "before"), and besides the step count a byte budget drops the oldest steps, so big
// projects cannot grow the history to gigabytes.
#include "project/Project.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace roy {

class UndoManager {
public:
    explicit UndoManager(Project& project) : project_(project) {}

    void begin(const std::string& name);
    // Returns true if the outermost transaction changed the project and was recorded.
    bool end();
    // Aborts the outermost transaction and restores the state from begin().
    void cancel();
    bool inTransaction() const { return depth_ > 0; }

    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoName() const { return undo_.empty() ? "" : undo_.back().name; }
    std::string redoName() const { return redo_.empty() ? "" : redo_.back().name; }
    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }
    void clear();
    void setLimit(size_t maxEntries) { limit_ = maxEntries; }
    // Oldest steps are dropped while the history needs more than this (the newest step always stays).
    void setMemoryLimit(size_t bytes) { memoryLimit_ = bytes; }
    size_t memoryBytes() const; // distinct stored states

    // Called after undo/redo/cancel restored a state (e.g. rebuild audio graph).
    std::function<void(const Project&)> onRestore;

private:
    using State = std::shared_ptr<const std::string>;
    void restore(const std::string& state);
    void trim();
    struct Entry {
        std::string name;
        State before;
        State after;
    };
    Project& project_;
    int depth_ = 0;
    std::string pendingName_;
    std::string before_;
    std::deque<Entry> undo_, redo_;
    size_t limit_ = 500;
    size_t memoryLimit_ = size_t{512} << 20;
};

} // namespace roy
