#include "diag/ClickDetector.h"

#include <algorithm>
#include <cmath>

namespace roy::diag {

ClickReport detectClicks(const float* x, size_t n, const ClickOptions& opt) {
    ClickReport r;
    if (!x || n < 3) return r;
    std::vector<double> e(n, 0.0), sq(n + 1, 0.0);
    for (size_t i = 2; i < n; ++i) e[i] = static_cast<double>(x[i]) - (2.0 * x[i - 1] - x[i - 2]);
    for (size_t i = 0; i < n; ++i) sq[i + 1] = sq[i] + e[i] * e[i];
    const int64_t W = std::max(8, opt.windowSamples);
    const int64_t N = static_cast<int64_t>(n);
    for (int64_t i = 2; i < N; ++i) {
        const double a = std::fabs(e[static_cast<size_t>(i)]);
        if (a < opt.absFloor) continue;
        // local level without the +-4 samples around i (a click spreads over 3 predictor samples)
        const int64_t lo = std::max<int64_t>(2, i - W), hi = std::min<int64_t>(N, i + W + 1);
        const int64_t elo = std::max<int64_t>(lo, i - 4), ehi = std::min<int64_t>(hi, i + 5);
        const double energy = (sq[static_cast<size_t>(hi)] - sq[static_cast<size_t>(lo)]) - (sq[static_cast<size_t>(ehi)] - sq[static_cast<size_t>(elo)]);
        const double cnt = static_cast<double>((hi - lo) - (ehi - elo));
        const double local = cnt > 0 ? std::sqrt(std::max(0.0, energy) / cnt) : 0.0;
        const double score = a / (local + opt.absFloor * 0.25);
        if (score > r.maxScore) {
            r.maxScore = score;
            r.maxScoreSample = i;
        }
        if (score < opt.threshold) continue;
        if (!r.events.empty() && i - r.events.back().sample < opt.mergeSamples) {
            if (score > r.events.back().score) r.events.back() = {r.events.back().sample, score, a};
            continue;
        }
        r.events.push_back({i, score, a});
    }
    return r;
}

double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b, size_t from) {
    const size_t n = std::min(a.size(), b.size());
    double m = a.size() == b.size() ? 0.0 : 1e9;
    for (size_t i = from; i < n; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}

} // namespace roy::diag
