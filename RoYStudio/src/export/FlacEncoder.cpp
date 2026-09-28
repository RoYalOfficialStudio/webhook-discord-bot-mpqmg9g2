#include "export/FlacEncoder.h"
#include "core/Files.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace roy::flac {

namespace {

class BitWriter {
public:
    std::vector<uint8_t> bytes;
    void put(uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc_ = static_cast<uint8_t>((acc_ << 1) | ((value >> i) & 1u));
            if (++n_ == 8) {
                bytes.push_back(acc_);
                acc_ = 0;
                n_ = 0;
            }
        }
    }
    void putSigned(int64_t v, int bits) { put(static_cast<uint64_t>(v) & ((bits == 64) ? ~0ull : ((1ull << bits) - 1)), bits); }
    void putUnary(uint32_t zeros) { // `zeros` zero bits followed by a one
        for (uint32_t i = 0; i < zeros; ++i) put(0, 1);
        put(1, 1);
    }
    void alignByte() {
        if (n_) put(0, 8 - n_);
    }
    size_t bitCount() const { return bytes.size() * 8 + static_cast<size_t>(n_); }

private:
    uint8_t acc_ = 0;
    int n_ = 0;
};

uint8_t crc8(const uint8_t* d, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1));
    }
    return crc;
}

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(d[i] << 8);
        for (int b = 0; b < 8; ++b) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : (crc << 1));
    }
    return crc;
}

void putUtf8(BitWriter& w, uint64_t v) {
    if (v < 0x80) {
        w.put(v, 8);
        return;
    }
    int bytes = 2;
    while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) ++bytes;
    const uint8_t first = static_cast<uint8_t>((0xFF00 >> bytes) & 0xFF) | static_cast<uint8_t>(v >> (6 * (bytes - 1)));
    w.put(first, 8);
    for (int i = bytes - 2; i >= 0; --i) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
}

inline uint32_t zigzag(int64_t r) { return static_cast<uint32_t>(r >= 0 ? (r << 1) : ((-r) << 1) - 1); }

// Cost in bits of Rice-coding `res` with parameter k.
uint64_t riceBits(const int64_t* res, size_t n, int k) {
    uint64_t bits = 0;
    for (size_t i = 0; i < n; ++i) bits += (zigzag(res[i]) >> k) + 1 + static_cast<uint64_t>(k);
    return bits;
}

int bestRiceParam(const int64_t* res, size_t n, uint64_t* bitsOut) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += zigzag(res[i]);
    int k = 0;
    if (n > 0) {
        const uint64_t mean = sum / n;
        while (k < 14 && (1ull << (k + 1)) <= mean + 1) ++k;
    }
    uint64_t best = riceBits(res, n, k);
    int bk = k;
    for (int d : {-1, 1}) {
        const int kk = k + d;
        if (kk < 0 || kk > 14) continue;
        const uint64_t b = riceBits(res, n, kk);
        if (b < best) {
            best = b;
            bk = kk;
        }
    }
    *bitsOut = best;
    return bk;
}

struct ResidualPlan {
    int partitionOrder = 0;
    std::vector<int> params;
    uint64_t bits = 0;
};

// residual has (blockSize - order) values; partition 0 omits the first `order` samples.
ResidualPlan planResidual(const std::vector<int64_t>& res, int blockSize, int order, int maxPo) {
    ResidualPlan best;
    best.bits = std::numeric_limits<uint64_t>::max();
    for (int po = 0; po <= maxPo; ++po) {
        const int parts = 1 << po;
        if (blockSize % parts) break;
        const int psize = blockSize / parts;
        if (psize <= order) break;
        ResidualPlan p;
        p.partitionOrder = po;
        p.bits = 2 + 4; // coding method + partition order
        size_t off = 0;
        for (int i = 0; i < parts; ++i) {
            const size_t n = static_cast<size_t>(i == 0 ? psize - order : psize);
            uint64_t b = 0;
            const int k = bestRiceParam(res.data() + off, n, &b);
            p.params.push_back(k);
            p.bits += 4 + b;
            off += n;
        }
        if (p.bits < best.bits) best = p;
    }
    return best;
}

void fixedResidual(const std::vector<int64_t>& x, int order, std::vector<int64_t>& r) {
    const size_t n = x.size();
    r.clear();
    for (size_t i = static_cast<size_t>(order); i < n; ++i) {
        int64_t pred = 0;
        switch (order) {
        case 0: pred = 0; break;
        case 1: pred = x[i - 1]; break;
        case 2: pred = 2 * x[i - 1] - x[i - 2]; break;
        case 3: pred = 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
        case 4: pred = 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
        }
        r.push_back(x[i] - pred);
    }
}

// Writes the best subframe for `x` (bps bits per sample). Returns bits used.
void writeSubframe(BitWriter& w, const std::vector<int64_t>& x, int bps, int maxPo) {
    const int n = static_cast<int>(x.size());
    bool constant = true;
    for (int i = 1; i < n && constant; ++i) constant = x[static_cast<size_t>(i)] == x[0];
    if (constant) {
        w.put(0, 1);
        w.put(0, 6); // SUBFRAME_CONSTANT
        w.put(0, 1);
        w.putSigned(x[0], bps);
        return;
    }
    uint64_t bestBits = static_cast<uint64_t>(n) * static_cast<uint64_t>(bps); // verbatim
    int bestOrder = -1;
    ResidualPlan bestPlan;
    std::vector<int64_t> r, bestRes;
    for (int order = 0; order <= 4 && order < n; ++order) {
        fixedResidual(x, order, r);
        auto plan = planResidual(r, n, order, maxPo);
        const uint64_t bits = static_cast<uint64_t>(order) * static_cast<uint64_t>(bps) + plan.bits;
        if (bits < bestBits) {
            bestBits = bits;
            bestOrder = order;
            bestPlan = plan;
            bestRes = r;
        }
    }
    w.put(0, 1);
    if (bestOrder < 0) {
        w.put(1, 6); // SUBFRAME_VERBATIM
        w.put(0, 1);
        for (int64_t v : x) w.putSigned(v, bps);
        return;
    }
    w.put(0x08 | static_cast<uint32_t>(bestOrder), 6); // SUBFRAME_FIXED
    w.put(0, 1);
    for (int i = 0; i < bestOrder; ++i) w.putSigned(x[static_cast<size_t>(i)], bps);
    w.put(0, 2); // residual coding method: 4-bit Rice parameters
    w.put(static_cast<uint32_t>(bestPlan.partitionOrder), 4);
    const int parts = 1 << bestPlan.partitionOrder;
    const int psize = n / parts;
    size_t off = 0;
    for (int p = 0; p < parts; ++p) {
        const int k = bestPlan.params[static_cast<size_t>(p)];
        w.put(static_cast<uint32_t>(k), 4);
        const size_t cnt = static_cast<size_t>(p == 0 ? psize - bestOrder : psize);
        for (size_t i = 0; i < cnt; ++i) {
            const uint32_t u = zigzag(bestRes[off + i]);
            w.putUnary(u >> k);
            if (k) w.put(u & ((1u << k) - 1), k);
        }
        off += cnt;
    }
}

} // namespace

std::vector<uint8_t> encodeToMemory(const std::vector<std::vector<int32_t>>& samples, int sampleRate, int bps, const EncodeOptions& opt) {
    const int channels = static_cast<int>(samples.size());
    const uint64_t total = samples.empty() ? 0 : samples[0].size();
    const int bs = std::clamp(opt.blockSize, 16, 65535);
    std::vector<uint8_t> out = {'f', 'L', 'a', 'C'};
    // STREAMINFO (last metadata block)
    {
        BitWriter w;
        w.put(1, 1);  // last block
        w.put(0, 7);  // STREAMINFO
        w.put(34, 24);
        w.put(static_cast<uint32_t>(bs), 16);
        w.put(static_cast<uint32_t>(bs), 16);
        w.put(0, 24); // min frame size unknown
        w.put(0, 24); // max frame size unknown
        w.put(static_cast<uint32_t>(sampleRate), 20);
        w.put(static_cast<uint32_t>(channels - 1), 3);
        w.put(static_cast<uint32_t>(bps - 1), 5);
        w.put(total, 36);
        for (int i = 0; i < 16; ++i) w.put(0, 8); // MD5 unknown (allowed)
        out.insert(out.end(), w.bytes.begin(), w.bytes.end());
    }
    uint64_t frameNo = 0;
    std::vector<std::vector<int64_t>> ch(static_cast<size_t>(channels));
    for (uint64_t pos = 0; pos < total; pos += static_cast<uint64_t>(bs), ++frameNo) {
        const int n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(bs), total - pos));
        for (int c = 0; c < channels; ++c) {
            ch[static_cast<size_t>(c)].resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) ch[static_cast<size_t>(c)][static_cast<size_t>(i)] = samples[static_cast<size_t>(c)][pos + static_cast<uint64_t>(i)];
        }
        // stereo decorrelation: pick the cheapest of independent / left-side / side-right / mid-side
        int assignment = channels - 1; // independent
        std::vector<std::vector<int64_t>> sub = ch;
        std::vector<int> subBps(static_cast<size_t>(channels), bps);
        if (channels == 2 && opt.stereoDecorrelation) {
            const auto& L = ch[0];
            const auto& R = ch[1];
            std::vector<int64_t> side(static_cast<size_t>(n)), mid(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                side[static_cast<size_t>(i)] = L[static_cast<size_t>(i)] - R[static_cast<size_t>(i)];
                mid[static_cast<size_t>(i)] = (L[static_cast<size_t>(i)] + R[static_cast<size_t>(i)]) >> 1;
            }
            auto cost = [&](const std::vector<int64_t>& x) {
                uint64_t c = 0;
                for (int i = 2; i < static_cast<int>(x.size()); ++i) {
                    const int64_t r = x[static_cast<size_t>(i)] - 2 * x[static_cast<size_t>(i - 1)] + x[static_cast<size_t>(i - 2)];
                    c += static_cast<uint64_t>(r < 0 ? -r : r);
                }
                return c;
            };
            const uint64_t cl = cost(L), cr = cost(R), cs = cost(side), cm = cost(mid);
            const uint64_t ind = cl + cr, ls = cl + cs, rs = cr + cs, ms = cm + cs;
            const uint64_t best = std::min({ind, ls, rs, ms});
            if (best == ms && ms < ind) {
                assignment = 10;
                sub = {mid, side};
                subBps = {bps, bps + 1};
            } else if (best == ls && ls < ind) {
                assignment = 8;
                sub = {L, side};
                subBps = {bps, bps + 1};
            } else if (best == rs && rs < ind) {
                assignment = 9;
                sub = {side, R};
                subBps = {bps + 1, bps};
            }
        }
        BitWriter w;
        w.put(0xFFF8, 16); // sync + reserved + fixed block size
        const bool full = n == bs && bs == 4096;
        w.put(full ? 0xC : 0x7, 4); // 4096 or 16-bit (n-1) at end of header
        w.put(0, 4);                // sample rate from STREAMINFO
        w.put(static_cast<uint32_t>(assignment), 4);
        w.put(0, 3); // sample size from STREAMINFO
        w.put(0, 1);
        putUtf8(w, frameNo);
        if (!full) w.put(static_cast<uint32_t>(n - 1), 16);
        w.put(crc8(w.bytes.data(), w.bytes.size()), 8);
        for (int c = 0; c < channels; ++c) writeSubframe(w, sub[static_cast<size_t>(c)], subBps[static_cast<size_t>(c)], opt.maxRicePartitionOrder);
        w.alignByte();
        const uint16_t crc = crc16(w.bytes.data(), w.bytes.size());
        w.put(crc, 16);
        out.insert(out.end(), w.bytes.begin(), w.bytes.end());
    }
    return out;
}

bool encode(const std::filesystem::path& path, const std::vector<std::vector<int32_t>>& samples, int sampleRate, int bps,
            const EncodeOptions& opt, std::string* error) {
    if (samples.empty() || samples.size() > 8 || (bps != 16 && bps != 24)) {
        if (error) *error = "FLAC: 1-8 channels, 16 or 24 bit";
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        if (error) *error = "refusing to overwrite " + path.string();
        return false;
    }
    const auto bytes = encodeToMemory(samples, sampleRate, bps, opt);
    return files::atomicWrite(path, std::string(bytes.begin(), bytes.end()), error);
}

} // namespace roy::flac
