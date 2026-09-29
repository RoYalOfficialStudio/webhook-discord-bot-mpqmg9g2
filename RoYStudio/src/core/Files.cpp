#include "core/Files.h"
#include "core/Process.h"

#include <cstdlib>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace roy::files {

namespace {

// ---- SHA-256 (FIPS 180-4), self-contained --------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    uint64_t total = 0;
    size_t used = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n > 0) {
            const size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string finish() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        std::string out;
        for (uint32_t v : h) out += std::format("{:08x}", v);
        return out;
    }
};

bool replaceFileImpl(const fs::path& from, const fs::path& to, std::string* error) {
#ifdef _WIN32
    if (!MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = std::format("MoveFileExW failed ({})", GetLastError());
        return false;
    }
    return true;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec && error) *error = ec.message();
    return !ec;
#endif
}

} // namespace

namespace {
struct DiskFault {
    std::mutex m;
    std::atomic<bool> active{false};
    std::string fragment;
    int64_t budget = 0;
};
DiskFault& diskFault() {
    static DiskFault f;
    return f;
}
// How many of `want` bytes may be written to `path` right now.
size_t allowedBytes(const fs::path& path, size_t want) {
    DiskFault& f = diskFault();
    if (!f.active.load(std::memory_order_acquire)) return want;
    std::lock_guard l(f.m);
    if (f.fragment.empty() || path.string().find(f.fragment) == std::string::npos) return want;
    const size_t ok = static_cast<size_t>(std::clamp<int64_t>(f.budget, 0, static_cast<int64_t>(want)));
    f.budget -= static_cast<int64_t>(ok);
    return ok;
}
} // namespace

namespace fault {
void setDiskFull(const std::string& pathFragment, int64_t bytesBudget) {
    DiskFault& f = diskFault();
    std::lock_guard l(f.m);
    f.fragment = pathFragment;
    f.budget = bytesBudget;
    f.active.store(!pathFragment.empty(), std::memory_order_release);
}
void clear() { setDiskFull({}, 0); }
} // namespace fault

bool replaceFile(const fs::path& from, const fs::path& to, std::string* error) {
    return replaceFileImpl(from, to, error);
}

FILE* openForWrite(const fs::path& path, bool append) {
#ifdef _WIN32
    return _wfopen(path.wstring().c_str(), append ? L"ab" : L"wb");
#else
    return std::fopen(path.string().c_str(), append ? "ab" : "wb");
#endif
}

size_t writeBytes(FILE* f, const void* data, size_t size, const fs::path& path) {
    if (!f || size == 0) return 0;
    const size_t allowed = allowedBytes(path, size);
    const size_t written = allowed > 0 ? std::fwrite(data, 1, allowed, f) : 0;
    if (written < size) errno = ENOSPC;
    return written;
}

bool atomicWrite(const fs::path& target, const std::string& contents, std::string* error) {
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    fs::path tmp = target;
    tmp += std::format(".tmp{}", newId().substr(0, 8));
    FILE* f = openForWrite(tmp);
    if (!f) {
        if (error) *error = "cannot open temp file " + tmp.string();
        return false;
    }
    const bool ok = writeBytes(f, contents.data(), contents.size(), tmp) == contents.size() && std::fflush(f) == 0;
#ifdef _WIN32
    _commit(_fileno(f));
#else
    ::fsync(fileno(f));
#endif
    std::fclose(f);
    if (!ok) {
        fs::remove(tmp, ec);
        if (error) *error = "write failed for " + tmp.string() + " (disk full or no permission?); the previous file is untouched";
        return false;
    }
    if (!replaceFileImpl(tmp, target, error)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::string> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path uniquePath(const fs::path& desired) {
    std::error_code ec;
    if (!fs::exists(desired, ec)) return desired;
    const auto stem = desired.stem().string();
    const auto ext = desired.extension().string();
    const auto dir = desired.parent_path();
    for (int i = 1; i < 1000000; ++i) {
        fs::path p = dir / std::format("{}_{:04d}{}", stem, i, ext);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / (stem + "_" + newId() + ext);
}

bool safeCopy(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (error) *error = "destination exists: " + to.string();
        return false;
    }
    if (to.has_parent_path()) fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

std::string sha256(const void* data, size_t size) {
    Sha256 s;
    s.update(static_cast<const uint8_t*>(data), size);
    return s.finish();
}

std::string sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    std::array<char, 65536> buf;
    while (in) {
        in.read(buf.data(), buf.size());
        const auto n = in.gcount();
        if (n > 0) s.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
    }
    return s.finish();
}

std::string nowIso8601() {
    using namespace std::chrono;
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", floor<seconds>(system_clock::now()));
}

std::string nowCompact() {
    using namespace std::chrono;
    return std::format("{:%Y%m%d_%H%M%S}", floor<seconds>(system_clock::now()));
}

std::string newId() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    return std::format("{:016x}{:016x}", rng(), rng());
}

bool portableMode() {
    static const bool on = [] {
        std::error_code ec;
        return fs::exists(fs::path(executableDirectory()) / "RoYStudio.portable", ec);
    }();
    return on;
}

fs::path defaultProjectsDirectory() {
    fs::path dir;
    if (portableMode()) {
        dir = fs::path(executableDirectory()) / "Projects";
    } else {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        dir = home && *home ? fs::path(home) / "Documents" / "RoY Studio Projects" : userDataDirectory() / "Projects";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path userDataDirectory() {
    auto env = [](const char* n) -> std::string {
        const char* v = std::getenv(n);
        return v ? v : "";
    };
    fs::path dir;
    if (auto o = env("ROY_USER_DIR"); !o.empty()) dir = o;
    else if (portableMode()) dir = fs::path(executableDirectory()) / "UserData";
#ifdef _WIN32
    else if (auto ad = env("APPDATA"); !ad.empty()) dir = fs::path(ad) / "RoYStudio";
#elif defined(__APPLE__)
    else if (auto h = env("HOME"); !h.empty()) dir = fs::path(h) / "Library" / "Application Support" / "RoYStudio";
#else
    else if (auto x = env("XDG_CONFIG_HOME"); !x.empty()) dir = fs::path(x) / "RoYStudio";
    else if (auto h = env("HOME"); !h.empty()) dir = fs::path(h) / ".config" / "RoYStudio";
#endif
    else dir = fs::temp_directory_path() / "RoYStudio";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace roy::files
