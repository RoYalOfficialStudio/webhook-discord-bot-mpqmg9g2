#pragma once
// Minimal ZIP writer (entries STORED, i.e. uncompressed; CRC-32; UTF-8 names). Enough for
// diagnostic packages that any OS can open without extra software. No external dependency.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace roy::zip {

uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

class Writer {
public:
    void add(const std::string& nameInZip, const std::string& contents);
    size_t entries() const { return files_.size(); }
    // Writes the archive atomically.
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    std::string bytes() const;

private:
    struct Entry {
        std::string name, data;
        uint32_t crc = 0;
    };
    std::vector<Entry> files_;
};

// Reads back the names + contents of a STORED zip written by Writer (tests, verification).
bool readStored(const std::filesystem::path& file, std::vector<std::pair<std::string, std::string>>& out);

} // namespace roy::zip
