#include "diag/ClickDetector.h"

#include <algorithm>
#include <cmath>

namespace roy::diag {
namespace {
constexpr int kOrder = 24, kHop = 512, kWin = 2048;

// autocorrelation LPC (Levinson-Durbin); returns a[1..p] with e[n] = x[n] + sum a[k] x[n-k]
bool lpc(const double* w, int n, double* a) {
    double r[kOrder + 1];
    for (int k = 0; k <= kOrder; ++k) {
        double s = 0;
        for (int i = k; i < n; ++i) s += w[i] * w[i - k];
        r[k] = s;
    }
    if (r[0] <= 1e-12) return false;
    r[0] *= 1.0 + 1e-6; // white-noise correction: keeps pure tones well conditioned
    for (int k = 1; k <= kOrder; ++k) r[k] *= std::exp(-0.5 * std::pow(0.01 * k, 2)); // lag window
    double tmp[kOrder + 1] = {};
    for (int i = 0; i <= kOrder; ++i) a[i] = 0;
    a[0] = 1;
    double err = r[0];
    for (int i = 1; i <= kOrder; ++i) {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const double k = -acc / err;
        for (int j = 0; j <= i; ++j) tmp[j] = a[j];
        for (int j = 1; j < i; ++j) a[j] = tmp[j] + k * tmp[i - j];
        a[i] = k;
        err *= (1 - k * k);
        if (err <= 1e-15) break;
    }
    return true;
}
} // namespace

ClickReport detectClicks(const float* x, size_t n, const ClickOptions& opt) {
    ClickReport r;
    if (!x || n < 3) return r;
    const int64_t N = static_cast<int64_t>(n);
    // 1. short-term residual
    std::vector<double> e1(n, 0.0), win(kWin);
    double a[kOrder + 1];
    for (int64_t h = 0; h < N; h += kHop) {
        const int64_t c = h + kHop / 2, s0 = c - kWin / 2;
        for (int i = 0; i < kWin; ++i) {
            const int64_t j = s0 + i;
            const double hann = 0.5 - 0.5 * std::cos(2 * 3.141592653589793 * (i + 0.5) / kWin);
            win[static_cast<size_t>(i)] = (j >= 0 && j < N) ? x[j] * hann : 0.0;
        }
        const bool ok = lpc(win.data(), kWin, a);
        for (int64_t i = h; i < std::min(N, h + kHop); ++i) {
            double e = x[i];
            if (ok)
                for (int k = 1; k <= kOrder; ++k) e += a[k] * (i - k >= 0 ? x[i - k] : 0.0f);
            e1[static_cast<size_t>(i)] = e;
        }
    }
    // 2. long-term (pitch) prediction, two-sided so a click leaves only half-size echoes at +-T
    std::vector<double> e2 = e1;
    std::vector<int> periodOf(static_cast<size_t>(N / kHop + 1), 0);
    constexpr int kMinT = 40, kMaxT = 800, kCorr = 1024;
    for (int64_t h = 0; h < N; h += kHop) {
        const int64_t c = h + kHop / 2;
        const int64_t lo = std::max<int64_t>(kMaxT, c - kCorr / 2), hi = std::min<int64_t>(N - kMaxT, c + kCorr / 2);
        if (hi - lo < 256) continue;
        double e0 = 0;
        for (int64_t i = lo; i < hi; ++i) e0 += e1[static_cast<size_t>(i)] * e1[static_cast<size_t>(i)];
        if (e0 <= 1e-12) continue;
        int bestT = 0;
        double bestC = 0.3;
        for (int T = kMinT; T <= kMaxT; ++T) {
            double xy = 0, yy = 0;
            for (int64_t i = lo; i < hi; ++i) {
                const double y = 0.5 * (e1[static_cast<size_t>(i - T)] + e1[static_cast<size_t>(i + T)]);
                xy += e1[static_cast<size_t>(i)] * y;
                yy += y * y;
            }
            const double cc = yy > 0 ? xy / std::sqrt(e0 * yy) : 0;
            if (cc > bestC) {
                bestC = cc;
                bestT = T;
            }
        }
        if (!bestT) continue;
        periodOf[static_cast<size_t>(h / kHop)] = bestT;
        double xy = 0, yy = 0;
        for (int64_t i = lo; i < hi; ++i) {
            const double y = 0.5 * (e1[static_cast<size_t>(i - bestT)] + e1[static_cast<size_t>(i + bestT)]);
            xy += e1[static_cast<size_t>(i)] * y;
            yy += y * y;
        }
        const double b = std::clamp(xy / yy, 0.0, 1.2);
        for (int64_t i = std::max<int64_t>(h, bestT); i < std::min(N - bestT, h + kHop); ++i)
            e2[static_cast<size_t>(i)] = e1[static_cast<size_t>(i)] - b * 0.5 * (e1[static_cast<size_t>(i - bestT)] + e1[static_cast<size_t>(i + bestT)]);
    }
    // 3. score against the local level
    std::vector<double> sq(n + 1, 0.0), score(n, 0.0);
    for (size_t i = 0; i < n; ++i) sq[i + 1] = sq[i] + e2[i] * e2[i];
    const int64_t W = std::max(8, opt.windowSamples);
    for (int64_t i = kOrder; i < N; ++i) {
        const double av = std::fabs(e2[static_cast<size_t>(i)]);
        if (av < opt.absFloor) continue;
        const int64_t lo = std::max<int64_t>(0, i - W), hi = std::min<int64_t>(N, i + W + 1);
        const int64_t elo = std::max<int64_t>(lo, i - 4), ehi = std::min<int64_t>(hi, i + 5);
        const double energy = (sq[static_cast<size_t>(hi)] - sq[static_cast<size_t>(lo)]) - (sq[static_cast<size_t>(ehi)] - sq[static_cast<size_t>(elo)]);
        const double cnt = static_cast<double>((hi - lo) - (ehi - elo));
        const double local = cnt > 0 ? std::sqrt(std::max(0.0, energy) / cnt) : 0.0;
        score[static_cast<size_t>(i)] = av / (local + opt.absFloor * 0.25);
    }
    // 4. a peak that repeats one period earlier AND later is a glottal pulse of a voice, not a click
    auto windowMax = [&](int64_t c, int r) {
        double m = 0;
        for (int64_t j = std::max<int64_t>(0, c - r); j <= std::min<int64_t>(N - 1, c + r); ++j) m = std::max(m, score[static_cast<size_t>(j)]);
        return m;
    };
    for (int64_t i = kOrder; i < N; ++i) {
        const double sc = score[static_cast<size_t>(i)];
        if (sc > r.maxScore) {
            r.maxScore = sc;
            r.maxScoreSample = i;
        }
        if (sc < opt.threshold) continue;
        const int T = periodOf[static_cast<size_t>(i / kHop)];
        if (T > 0) {
            // (the click's own echoes from step 2 are at +-T with at most ~0.6 x its score)
            const int tol = std::max(3, T / 10);
            const bool before = i - T >= 0 && windowMax(i - T, tol) >= 0.7 * sc;
            const bool after = i + T < N && windowMax(i + T, tol) >= 0.7 * sc;
            if (before && after) continue;
        }
        const double av = std::fabs(e2[static_cast<size_t>(i)]);
        if (!r.events.empty() && i - r.events.back().sample < opt.mergeSamples) {
            if (sc > r.events.back().score) r.events.back() = {r.events.back().sample, sc, av};
            continue;
        }
        r.events.push_back({i, sc, av});
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
