#include "diag/LatencyProbe.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::diag {
namespace {
constexpr double kBurst = 0.1, kGap = 0.6;
constexpr int kBursts = 3;
} // namespace

std::vector<float> makeLatencyProbe(double sr) {
    std::vector<float> p(static_cast<size_t>((kGap * (kBursts - 1) + kBurst) * sr), 0.0f);
    Rng rng(20261028);
    const int len = static_cast<int>(kBurst * sr);
    for (int b = 0; b < kBursts; ++b) {
        const size_t start = static_cast<size_t>(b * kGap * sr);
        for (int i = 0; i < len; ++i) {
            const double fade = std::min(1.0, std::min(i, len - 1 - i) / (0.002 * sr)); // 2 ms fades
            p[start + static_cast<size_t>(i)] = static_cast<float>(0.25 * fade * rng.uniform(-1.0, 1.0)); // about -12 dBFS
        }
    }
    return p;
}

double latencyProbeSeconds() { return kGap * (kBursts - 1) + kBurst + 0.6; }

LatencyResult analyseLatencyProbe(const std::vector<float>& probe, const std::vector<float>& rec, double sr, double maxLatency) {
    LatencyResult r;
    const int len = static_cast<int>(kBurst * sr), maxLag = static_cast<int>(maxLatency * sr);
    double minConf = 1.0;
    for (int b = 0; b < kBursts; ++b) {
        const size_t start = static_cast<size_t>(b * kGap * sr);
        if (start + static_cast<size_t>(len) > probe.size()) break;
        double pp = 0;
        for (int i = 0; i < len; ++i) pp += static_cast<double>(probe[start + static_cast<size_t>(i)]) * probe[start + static_cast<size_t>(i)];
        double best = -1;
        int bestLag = -1;
        // running energy of the recording window
        double rr = 0;
        for (int i = 0; i < len && start + static_cast<size_t>(i) < rec.size(); ++i) rr += static_cast<double>(rec[start + static_cast<size_t>(i)]) * rec[start + static_cast<size_t>(i)];
        for (int lag = 0; lag <= maxLag; ++lag) {
            const size_t o = start + static_cast<size_t>(lag);
            if (o + static_cast<size_t>(len) > rec.size()) break;
            if (lag > 0) {
                const double out = rec[o - 1], in = rec[o + static_cast<size_t>(len) - 1];
                rr += in * in - out * out;
            }
            double xy = 0;
            for (int i = 0; i < len; ++i) xy += static_cast<double>(probe[start + static_cast<size_t>(i)]) * rec[o + static_cast<size_t>(i)];
            const double c = rr > 1e-12 ? std::fabs(xy) / std::sqrt(pp * rr) : 0.0;
            if (c > best) {
                best = c;
                bestLag = lag;
            }
        }
        r.lags.push_back(bestLag);
        minConf = std::min(minConf, std::max(0.0, best));
    }
    if (r.lags.size() != kBursts) {
        r.message = "recording too short";
        return r;
    }
    const auto [mn, mx] = std::minmax_element(r.lags.begin(), r.lags.end());
    r.spread = *mx - *mn;
    r.confidence = minConf;
    std::vector<int> s = r.lags;
    std::sort(s.begin(), s.end());
    r.samples = s[1];
    r.ms = r.samples * 1000.0 / sr;
    if (r.confidence < 0.3) {
        r.message = std::format("no clear echo of the test signal (correlation {:.2f}) - is output connected to input?", r.confidence);
    } else if (r.spread > 2) {
        r.message = std::format("the three measurements disagree by {} samples - noise or echo; try again", r.spread);
    } else {
        r.ok = true;
        r.message = std::format("round-trip latency {} samples = {:.2f} ms (correlation {:.2f})", r.samples, r.ms, r.confidence);
    }
    return r;
}

} // namespace roy::diag
