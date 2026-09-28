#include "beat/Collision.h"
#include "core/Math.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::beat {

nlohmann::json CollisionReport::toJson() const {
    nlohmann::json s = nlohmann::json::array();
    for (auto& x : suggestions) s.push_back({{"type", x.type}, {"description", x.description}, {"params", x.params}});
    return {{"kickFundamentalHz", kickFundamentalHz}, {"bassFundamentalHz", bassFundamentalHz}, {"overlapRatio", overlapRatio},
            {"correlation", correlation}, {"sumVsSeparateDb", sumVsSeparateDb}, {"kickPeakDb", kickPeakDb},
            {"bassPeakDb", bassPeakDb}, {"combinedPeakDb", combinedPeakDb}, {"kickLowDb", kickLowDb}, {"bassLowDb", bassLowDb},
            {"combinedLowDb", combinedLowDb}, {"kickDecayMs", kickDecayMs}, {"bestBassDelayMs", bestBassDelayMs},
            {"invertBassHelps", invertBassHelps}, {"suggestions", s}};
}

double lowFundamental(const float* x, int64_t n, double sr, double skip) {
    const int64_t a = std::min<int64_t>(n, static_cast<int64_t>(skip * sr));
    const int N = 16384;
    std::vector<float> seg(N, 0.0f);
    for (int i = 0; i < N && a + i < n; ++i) seg[static_cast<size_t>(i)] = x[a + i];
    auto mag = dsp::magnitudeSpectrum(seg.data(), N);
    size_t best = 0;
    for (size_t b = static_cast<size_t>(25.0 * N / sr); b < static_cast<size_t>(250.0 * N / sr) && b + 1 < mag.size(); ++b)
        if (mag[b] > mag[best] || best == 0) best = b;
    if (best == 0) return 0;
    const double y0 = mag[best - 1], y1 = mag[best], y2 = mag[best + 1];
    const double den = y0 - 2 * y1 + y2;
    const double d = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
    return (static_cast<double>(best) + d) * sr / N;
}

namespace {
std::vector<float> lowBand(const std::vector<float>& x, double sr) {
    dsp::Biquad a, b;
    a.set(dsp::Biquad::Type::LowPass, sr, 150.0, 0.707);
    b.set(dsp::Biquad::Type::LowPass, sr, 150.0, 0.707);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = b.process(a.process(x[i]));
    return y;
}
double energy(const std::vector<float>& x) {
    double e = 0;
    for (float v : x) e += static_cast<double>(v) * v;
    return e;
}
double peakDb(const std::vector<float>& x) {
    float p = 0;
    for (float v : x) p = std::max(p, std::fabs(v));
    return gainToDb(static_cast<double>(p));
}
} // namespace

CollisionReport analyzeKick808(const std::vector<float>& kickIn, const std::vector<float>& bassIn, double sr, double bassOffset) {
    CollisionReport r;
    const int64_t off = static_cast<int64_t>(std::llround(bassOffset * sr));
    const size_t len = std::max(kickIn.size(), static_cast<size_t>(std::max<int64_t>(0, off + static_cast<int64_t>(bassIn.size()))));
    std::vector<float> kick(len, 0.0f), bass(len, 0.0f);
    std::copy(kickIn.begin(), kickIn.end(), kick.begin());
    for (size_t i = 0; i < bassIn.size(); ++i) {
        const int64_t d = off + static_cast<int64_t>(i);
        if (d >= 0 && static_cast<size_t>(d) < len) bass[static_cast<size_t>(d)] = bassIn[i];
    }
    r.kickFundamentalHz = lowFundamental(kickIn.data(), static_cast<int64_t>(kickIn.size()), sr, 0.02);
    r.bassFundamentalHz = lowFundamental(bassIn.data(), static_cast<int64_t>(bassIn.size()), sr, 0.03);
    auto kl = lowBand(kick, sr), bl = lowBand(bass, sr);
    std::vector<float> sum(len), sumL(len);
    for (size_t i = 0; i < len; ++i) {
        sum[i] = kick[i] + bass[i];
        sumL[i] = kl[i] + bl[i];
    }
    r.kickPeakDb = peakDb(kick);
    r.bassPeakDb = peakDb(bass);
    r.combinedPeakDb = peakDb(sum);
    const double eK = energy(kl), eB = energy(bl), eS = energy(sumL);
    r.kickLowDb = 10 * std::log10(eK + 1e-20);
    r.bassLowDb = 10 * std::log10(eB + 1e-20);
    r.combinedLowDb = 10 * std::log10(eS + 1e-20);
    r.sumVsSeparateDb = 10 * std::log10((eS + 1e-20) / (eK + eB + 1e-20));

    // temporal overlap using 5 ms envelopes of the low bands
    const size_t w = static_cast<size_t>(0.005 * sr);
    double both = 0, kTot = 0, kPeakEnv = 0;
    std::vector<double> kEnv;
    for (size_t i = 0; i < len; i += w) {
        double ek = 0, eb = 0;
        for (size_t k = i; k < std::min(len, i + w); ++k) {
            ek += static_cast<double>(kl[k]) * kl[k];
            eb += static_cast<double>(bl[k]) * bl[k];
        }
        kTot += ek;
        both += std::min(ek, eb);
        kEnv.push_back(ek);
        kPeakEnv = std::max(kPeakEnv, ek);
    }
    r.overlapRatio = kTot > 0 ? std::clamp(both / kTot, 0.0, 1.0) : 0.0;
    size_t peakIdx = 0;
    for (size_t i = 0; i < kEnv.size(); ++i)
        if (kEnv[i] >= kPeakEnv) { peakIdx = i; break; }
    for (size_t i = peakIdx; i < kEnv.size(); ++i)
        if (kEnv[i] < kPeakEnv * 0.01) { // -20 dB
            r.kickDecayMs = static_cast<double>(i - peakIdx) * 5.0;
            break;
        }
    // phase relation during the overlap: short-window correlation weighted by the overlap energy
    {
        const size_t cw = static_cast<size_t>(0.01 * sr);
        double num = 0, den = 0;
        for (size_t i = 0; i + cw <= len; i += cw) {
            double sxy = 0, sxx = 0, syy = 0;
            for (size_t k = i; k < i + cw; ++k) {
                sxy += static_cast<double>(kl[k]) * bl[k];
                sxx += static_cast<double>(kl[k]) * kl[k];
                syy += static_cast<double>(bl[k]) * bl[k];
            }
            if (sxx <= 1e-12 || syy <= 1e-12) continue;
            const double wgt = std::min(sxx, syy);
            num += wgt * sxy / std::sqrt(sxx * syy);
            den += wgt;
        }
        r.correlation = den > 0 ? num / den : 0.0;
    }
    // best delay / polarity for maximum combined low energy
    double bestE = eS;
    const int maxD = static_cast<int>(0.015 * sr);
    const int step = std::max(1, static_cast<int>(sr / 48000.0 * 12));
    for (int pol = 1; pol >= -1; pol -= 2)
        for (int d = 0; d <= maxD; d += step) {
            double e = 0;
            for (size_t i = 0; i < len; ++i) {
                const float b = static_cast<int64_t>(i) - d >= 0 ? bl[i - static_cast<size_t>(d)] * static_cast<float>(pol) : 0.0f;
                const double s = kl[i] + b;
                e += s * s;
            }
            if (e > bestE * 1.02) {
                bestE = e;
                r.bestBassDelayMs = d / sr * 1000.0;
                r.invertBassHelps = pol < 0;
            }
        }

    // ---- suggestions (nothing is changed automatically) ----
    if (r.overlapRatio > 0.25) {
        r.suggestions.push_back({"sidechain",
                                 std::format("Kick and 808 overlap ({:.0f} % of the kick's low energy). Duck the 808 with the kick as sidechain.",
                                             r.overlapRatio * 100),
                                 {{"typeId", "roy.compressor"}, {"params", {{"ratio", 4.0}, {"attack", 1.0}, {"release", std::clamp(r.kickDecayMs, 40.0, 400.0)}, {"threshold", -24.0}}},
                                  {"sidechain", "kick"}}});
    }
    const bool closeFreq = r.kickFundamentalHz > 0 && r.bassFundamentalHz > 0 &&
                           std::fabs(12.0 * std::log2(r.kickFundamentalHz / r.bassFundamentalHz)) < 4.0;
    if (closeFreq && r.overlapRatio > 0.15) {
        r.suggestions.push_back({"dynamic_eq",
                                 std::format("Fundamentals are close (kick {:.0f} Hz, 808 {:.0f} Hz). Cut the 808 around {:.0f} Hz only while the kick hits.",
                                             r.kickFundamentalHz, r.bassFundamentalHz, r.kickFundamentalHz),
                                 {{"typeId", "roy.dynamicspace"}, {"params", {{"frequency", std::round(r.kickFundamentalHz)}, {"bandwidth", 0.7}, {"maxReduction", -6.0}}},
                                  {"sidechain", "kick"}}});
    }
    if (r.kickDecayMs > 250.0 && r.overlapRatio > 0.2) {
        r.suggestions.push_back({"envelope", std::format("Long kick tail ({:.0f} ms) runs into the 808. Shorten the kick decay or start the 808 later.", r.kickDecayMs),
                                 {{"kickDecayMs", std::round(std::min(r.kickDecayMs, 220.0))}}});
    }
    if (r.sumVsSeparateDb < -1.0 || r.correlation < -0.2 || r.invertBassHelps || r.bestBassDelayMs > 0.5) {
        r.suggestions.push_back({"phase",
                                 std::format("Low-end cancellation of {:.1f} dB (correlation {:.2f}). {}", r.sumVsSeparateDb, r.correlation,
                                             r.invertBassHelps ? "Inverting the 808 polarity increases the low end."
                                                               : std::format("Delaying the 808 by {:.1f} ms increases the low end.", r.bestBassDelayMs)),
                                 {{"invertPolarity", r.invertBassHelps}, {"delayMs", r.bestBassDelayMs}}});
    }
    return r;
}

} // namespace roy::beat
