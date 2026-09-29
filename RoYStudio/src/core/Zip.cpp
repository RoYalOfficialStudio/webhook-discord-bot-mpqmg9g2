#include "core/Zip.h"
#include "core/Files.h"

#include <array>
#include <ctime>
#include <fstream>
#include <sstream>

namespace roy::zip {

namespace {
const std::array<uint32_t, 256>& table() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            a[i] = c;
        }
        return a;
    }();
    return t;
}
void u16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void u32(std::string& s, uint32_t v) {
    u16(s, v & 0xFFFF);
    u16(s, v >> 16);
}
uint32_t rd16(const std::string& s, size_t p) { return static_cast<uint8_t>(s[p]) | static_cast<uint32_t>(static_cast<uint8_t>(s[p + 1])) << 8; }
uint32_t rd32(const std::string& s, size_t p) { return rd16(s, p) | rd16(s, p + 2) << 16; }
} // namespace

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    const auto& t = table();
    crc = ~crc;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Writer::add(const std::string& name, const std::string& contents) { files_.push_back({name, contents, crc32(contents.data(), contents.size())}); }

std::string Writer::bytes() const {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const uint32_t dosTime = static_cast<uint32_t>(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const uint32_t dosDate = static_cast<uint32_t>((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    std::string out, central;
    for (auto& e : files_) {
        const uint32_t offset = static_cast<uint32_t>(out.size());
        // local file header
        u32(out, 0x04034b50);
        u16(out, 20);          // version needed
        u16(out, 0x0800);      // UTF-8 names
        u16(out, 0);           // stored
        u16(out, dosTime);
        u16(out, dosDate);
        u32(out, e.crc);
        u32(out, static_cast<uint32_t>(e.data.size()));
        u32(out, static_cast<uint32_t>(e.data.size()));
        u16(out, static_cast<uint32_t>(e.name.size()));
        u16(out, 0);
        out += e.name;
        out += e.data;
        // central directory record
        u32(central, 0x02014b50);
        u16(central, 20);
        u16(central, 20);
        u16(central, 0x0800);
        u16(central, 0);
        u16(central, dosTime);
        u16(central, dosDate);
        u32(central, e.crc);
        u32(central, static_cast<uint32_t>(e.data.size()));
        u32(central, static_cast<uint32_t>(e.data.size()));
        u16(central, static_cast<uint32_t>(e.name.size()));
        u16(central, 0); // extra
        u16(central, 0); // comment
        u16(central, 0); // disk
        u16(central, 0); // internal attrs
        u32(central, 0); // external attrs
        u32(central, offset);
        central += e.name;
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<uint32_t>(files_.size()));
    u16(out, static_cast<uint32_t>(files_.size()));
    u32(out, static_cast<uint32_t>(central.size()));
    u32(out, cdOffset);
    u16(out, 0);
    return out;
}

bool Writer::save(const std::filesystem::path& file, std::string* error) const { return files::atomicWrite(file, bytes(), error); }

bool readStored(const std::filesystem::path& file, std::vector<std::pair<std::string, std::string>>& out) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    size_t p = 0;
    while (p + 30 <= s.size() && rd32(s, p) == 0x04034b50) {
        if (rd16(s, p + 8) != 0) return false; // only stored entries
        const uint32_t crc = rd32(s, p + 14), size = rd32(s, p + 18);
        const uint32_t nameLen = rd16(s, p + 26), extraLen = rd16(s, p + 28);
        if (p + 30 + nameLen + extraLen + size > s.size()) return false;
        std::string name = s.substr(p + 30, nameLen);
        std::string data = s.substr(p + 30 + nameLen + extraLen, size);
        if (crc32(data.data(), data.size()) != crc) return false;
        out.emplace_back(std::move(name), std::move(data));
        p += 30 + nameLen + extraLen + size;
    }
    return !out.empty() && p + 4 <= s.size() && rd32(s, p) == 0x02014b50;
}

} // namespace roy::zip
