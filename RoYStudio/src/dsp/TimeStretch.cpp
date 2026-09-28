#include "dsp/TimeStretch.h"
#include "core/Math.h"
#include "dsp/Resampler.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

Channels timeStretch(const Channels& in, double ratio, double sr) {
    if (in.empty() || in[0].empty()) return in;
    if (std::fabs(ratio - 1.0) < 1e-6) return in;
    ratio = std::clamp(ratio, 0.1, 10.0);
    const size_t nCh = in.size();
    const int64_t inLen = static_cast<int64_t>(in[0].size());
    const int N = std::max(256, static_cast<int>(std::lround(0.030 * sr)) & ~1); // frame
    const int Hs = N / 2;                                                        // synthesis hop
    const int tol = Hs / 2;                                                      // search range
    const int64_t outLen = static_cast<int64_t>(std::llround(static_cast<double>(inLen) * ratio));

    // periodic Hann window: sums to 1 at 50% overlap
    std::vector<float> win(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / N));

    // mono guide signal for alignment
    std::vector<float> guide(static_cast<size_t>(inLen));
    for (size_t c = 0; c < nCh; ++c)
        for (int64_t i = 0; i < inLen; ++i) guide[static_cast<size_t>(i)] += in[c][static_cast<size_t>(i)] / static_cast<float>(nCh);
    auto g = [&](int64_t i) { return (i >= 0 && i < inLen) ? guide[static_cast<size_t>(i)] : 0.0f; };

    Channels out(nCh, std::vector<float>(static_cast<size_t>(outLen + N), 0.0f));
    std::vector<float> norm(static_cast<size_t>(outLen + N), 0.0f);

    int64_t prevPos = 0;
    for (int64_t k = 0;; ++k) {
        const int64_t outPos = k * Hs;
        if (outPos >= outLen) break;
        const int64_t nominal = static_cast<int64_t>(std::llround(static_cast<double>(outPos) / ratio));
        int64_t best = nominal;
        if (k > 0) {
            const int64_t natural = prevPos + Hs;
            // coarse search (step 4, decimated correlation), then fine refine
            double bestScore = -1e300;
            auto score = [&](int64_t cand, int step) {
                double xy = 0, xx = 0, yy = 0;
                for (int i = 0; i < N; i += step) {
                    const double x = g(cand + i), y = g(natural + i);
                    xy += x * y;
                    xx += x * x;
                    yy += y * y;
                }
                return xy / std::sqrt(xx * yy + 1e-12);
            };
            for (int d = -tol; d <= tol; d += 4) {
                const double s = score(nominal + d, 4);
                if (s > bestScore) {
                    bestScore = s;
                    best = nominal + d;
                }
            }
            const int64_t coarse = best;
            bestScore = -1e300;
            for (int d = -4; d <= 4; ++d) {
                const double s = score(coarse + d, 2);
                if (s > bestScore) {
                    bestScore = s;
                    best = coarse + d;
                }
            }
        }
        best = std::clamp<int64_t>(best, 0, std::max<int64_t>(0, inLen - 1));
        for (int i = 0; i < N; ++i) {
            const int64_t src = best + i;
            const size_t dst = static_cast<size_t>(outPos + i);
            if (dst >= norm.size()) break;
            const float w = win[static_cast<size_t>(i)];
            norm[dst] += w;
            if (src < inLen)
                for (size_t c = 0; c < nCh; ++c) out[c][dst] += in[c][static_cast<size_t>(src)] * w;
        }
        prevPos = best;
    }
    for (size_t c = 0; c < nCh; ++c) {
        out[c].resize(static_cast<size_t>(outLen));
        for (int64_t i = 0; i < outLen; ++i) {
            const float n = norm[static_cast<size_t>(i)];
            if (n > 1e-3f) out[c][static_cast<size_t>(i)] /= n;
        }
    }
    return out;
}

Channels stretchAndShift(const Channels& in, double ratio, double semitones, double sr) {
    if (std::fabs(semitones) < 1e-6) return timeStretch(in, ratio, sr);
    const double f = std::pow(2.0, semitones / 12.0);
    Channels st = timeStretch(in, ratio * f, sr);
    for (auto& c : st) c = resample(c, sr * f, sr);
    // exact target length
    const size_t target = static_cast<size_t>(std::llround(static_cast<double>(in.empty() ? 0 : in[0].size()) * ratio));
    for (auto& c : st) c.resize(target, 0.0f);
    return st;
}

Channels pitchShift(const Channels& in, double semitones, double sr) { return stretchAndShift(in, 1.0, semitones, sr); }

} // namespace roy::dsp
