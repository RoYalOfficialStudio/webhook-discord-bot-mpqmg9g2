#pragma once
// File-system helpers with a "never lose data" bias:
// * atomicWrite writes to a temp file, flushes, then renames over the target.
// * uniquePath never returns an existing path, so nothing is overwritten.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace roy::files {

namespace fs = std::filesystem;

bool atomicWrite(const fs::path& target, const std::string& contents, std::string* error = nullptr);
std::optional<std::string> readAll(const fs::path& path);
// "base.ext" -> "base.ext" if free, else "base_0001.ext", "base_0002.ext", ...
fs::path uniquePath(const fs::path& desired);
// Copies a file without ever overwriting an existing destination.
bool safeCopy(const fs::path& from, const fs::path& to, std::string* error = nullptr);
std::string sha256File(const fs::path& path);
std::string sha256(const void* data, size_t size);
std::string nowIso8601();
std::string nowCompact(); // 20260928_153200
std::string newId();      // 128-bit random hex id
// Per-user settings folder: %APPDATA%\\RoYStudio (Windows), ~/.config/RoYStudio (Linux),
// ~/Library/Application Support/RoYStudio (macOS). Created on demand. ROY_USER_DIR overrides it.
fs::path userDataDirectory();

// Atomically renames `from` over `to` (replacing it).
bool replaceFile(const fs::path& from, const fs::path& to, std::string* error = nullptr);
// fopen(path, "wb") that also handles non-ASCII paths on Windows (_wfopen).
FILE* openForWrite(const fs::path& path, bool append = false);
// fwrite() through the disk-fault injector below. Returns bytes actually written.
size_t writeBytes(FILE* f, const void* data, size_t size, const fs::path& path);

// ---- FAULT INJECTION (TESTS ONLY) ----------------------------------------------
// Simulates a full / failing disk for every write through atomicWrite / writeBytes
// whose path contains `pathFragment`: after `bytesBudget` more bytes, writes come
// back short. An empty fragment disables the fault. Never enabled by the product.
namespace fault {
void setDiskFull(const std::string& pathFragment, int64_t bytesBudget = 0);
void clear();
} // namespace fault

} // namespace roy::files
