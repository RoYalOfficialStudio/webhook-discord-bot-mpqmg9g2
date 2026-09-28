#pragma once
// Minimal base64 (RFC 4648) used to carry opaque plugin state inside JSON.
#include <cstdint>
#include <string>
#include <vector>

namespace roy::b64 {

inline std::string encode(const void* data, size_t size) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < size) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < size) v |= uint32_t(p[i + 1]) << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += i + 1 < size ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Returns false on invalid input.
inline bool decode(const std::string& s, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (s.size() % 4) return false;
    out.reserve(s.size() / 4 * 3);
    for (size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + static_cast<size_t>(k)];
            if (c == '=') {
                if (i + 4 != s.size() || k < 2) return false;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return false;
                v[k] = val(c);
                if (v[k] < 0) return false;
            }
        }
        const uint32_t x = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out.push_back(static_cast<uint8_t>(x >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>((x >> 8) & 255));
        if (pad < 1) out.push_back(static_cast<uint8_t>(x & 255));
    }
    return true;
}

} // namespace roy::b64
