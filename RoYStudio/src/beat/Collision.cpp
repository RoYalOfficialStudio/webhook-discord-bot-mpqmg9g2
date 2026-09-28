#include "beat/Collision.h"
#include "core/Math.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

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

// ---------------------------------------------------------------- visual analyzer
nlohmann::json Kick808Visual::toJson() const {
    auto arr = [](const std::vector<float>& v) {
        nlohmann::json a = nlohmann::json::array();
        for (float x : v) a.push_back(std::isfinite(x) ? nlohmann::json(std::round(x * 100.0f) / 100.0f) : nlohmann::json(nullptr));
        return a;
    };
    return {{"timeMs", arr(timeMs)}, {"kickEnvDb", arr(kickEnvDb)}, {"bassEnvDb", arr(bassEnvDb)}, {"correlation", arr(correlation)},
            {"freqHz", arr(freqHz)}, {"kickSpecDb", arr(kickSpecDb)}, {"bassSpecDb", arr(bassSpecDb)},
            {"frequencyOverlap", frequencyOverlap}, {"timingOverlapMs", timingOverlapMs}, {"phaseCorrelation", phaseCorrelation}};
}

Kick808Visual analyzeKick808Visual(const std::vector<float>& kickIn, const std::vector<float>& bassIn, double sr, double bassOffset,
                                   double lengthSec) {
    Kick808Visual v;
    const size_t len = static_cast<size_t>(std::max(0.05, lengthSec) * sr);
    const int64_t off = static_cast<int64_t>(std::llround(bassOffset * sr));
    std::vector<float> kick(len, 0.0f), bass(len, 0.0f);
    for (size_t i = 0; i < std::min(len, kickIn.size()); ++i) kick[i] = kickIn[i];
    for (size_t i = 0; i < bassIn.size(); ++i) {
        const int64_t d = off + static_cast<int64_t>(i);
        if (d >= 0 && static_cast<size_t>(d) < len) bass[static_cast<size_t>(d)] = bassIn[i];
    }
    auto kl = lowBand(kick, sr), bl = lowBand(bass, sr);
    // envelopes + running correlation
    const size_t hop = std::max<size_t>(1, static_cast<size_t>(0.005 * sr)), cw = static_cast<size_t>(0.02 * sr);
    std::vector<double> ek, eb;
    const size_t ew = static_cast<size_t>(0.025 * sr); // RMS window longer than one period of a 40 Hz sub
    for (size_t i = 0; i + hop <= len; i += hop) {
        double a = 0, b = 0;
        const size_t e0 = i >= ew / 2 ? i - ew / 2 : 0, e1 = std::min(len, e0 + ew);
        for (size_t k = e0; k < e1; ++k) {
            a += static_cast<double>(kl[k]) * kl[k];
            b += static_cast<double>(bl[k]) * bl[k];
        }
        ek.push_back(a / static_cast<double>(std::max<size_t>(1, e1 - e0)));
        eb.push_back(b / static_cast<double>(std::max<size_t>(1, e1 - e0)));
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t k = i; k < std::min(len, i + cw); ++k) {
            sxy += static_cast<double>(kl[k]) * bl[k];
            sxx += static_cast<double>(kl[k]) * kl[k];
            syy += static_cast<double>(bl[k]) * bl[k];
        }
        const bool sounding = sxx > 1e-9 && syy > 1e-9;
        v.correlation.push_back(sounding ? static_cast<float>(sxy / std::sqrt(sxx * syy)) : std::numeric_limits<float>::quiet_NaN());
        v.timeMs.push_back(static_cast<float>(1000.0 * static_cast<double>(i) / sr));
    }
    const double pk = std::max({1e-20, *std::max_element(ek.begin(), ek.end()), *std::max_element(eb.begin(), eb.end())});
    const double pkK = std::max(1e-20, *std::max_element(ek.begin(), ek.end()));
    const double pkB = std::max(1e-20, *std::max_element(eb.begin(), eb.end()));
    double both = 0, num = 0, den = 0;
    for (size_t i = 0; i < ek.size(); ++i) {
        v.kickEnvDb.push_back(static_cast<float>(10.0 * std::log10(ek[i] / pk + 1e-12)));
        v.bassEnvDb.push_back(static_cast<float>(10.0 * std::log10(eb[i] / pk + 1e-12)));
        const bool k20 = ek[i] > pkK * 0.01, b20 = eb[i] > pkB * 0.01;
        if (k20 && b20) {
            both += 5.0;
            if (std::isfinite(v.correlation[i])) {
                const double w = std::min(ek[i], eb[i]);
                num += w * v.correlation[i];
                den += w;
            }
        }
    }
    v.timingOverlapMs = both;
    v.phaseCorrelation = den > 0 ? num / den : 0.0;
    // spectra of the first 16384 samples (zero padded)
    const int N = 16384;
    std::vector<float> ks(N, 0.0f), bs(N, 0.0f);
    for (int i = 0; i < N && static_cast<size_t>(i) < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / (std::min<double>(N, static_cast<double>(len)) - 1));
        ks[static_cast<size_t>(i)] = static_cast<float>(kick[static_cast<size_t>(i)] * w);
        bs[static_cast<size_t>(i)] = static_cast<float>(bass[static_cast<size_t>(i)] * w);
    }
    auto km = dsp::magnitudeSpectrum(ks.data(), N), bm = dsp::magnitudeSpectrum(bs.data(), N);
    const int bins = 96;
    std::vector<double> kb(bins), bb(bins);
    double specPk = 1e-20, shared = 0, kTot = 0;
    for (int b = 0; b < bins; ++b) {
        const double f0 = 20.0 * std::pow(20.0, static_cast<double>(b) / bins), f1 = 20.0 * std::pow(20.0, static_cast<double>(b + 1) / bins);
        const size_t i0 = static_cast<size_t>(f0 * N / sr), i1 = std::max(i0 + 1, static_cast<size_t>(f1 * N / sr));
        double a = 0, c = 0;
        for (size_t i = i0; i < i1 && i < km.size(); ++i) {
            a += static_cast<double>(km[i]) * km[i];
            c += static_cast<double>(bm[i]) * bm[i];
        }
        kb[static_cast<size_t>(b)] = a;
        bb[static_cast<size_t>(b)] = c;
        specPk = std::max({specPk, a, c});
        shared += std::min(a, c);
        kTot += a;
        v.freqHz.push_back(static_cast<float>(std::sqrt(f0 * f1)));
    }
    for (int b = 0; b < bins; ++b) {
        v.kickSpecDb.push_back(static_cast<float>(10.0 * std::log10(kb[static_cast<size_t>(b)] / specPk + 1e-12)));
        v.bassSpecDb.push_back(static_cast<float>(10.0 * std::log10(bb[static_cast<size_t>(b)] / specPk + 1e-12)));
    }
    v.frequencyOverlap = kTot > 0 ? std::clamp(shared / kTot, 0.0, 1.0) : 0.0;
    return v;
}

// ---------------------------------------------------------------- root detection
nlohmann::json RootDetection::toJson() const {
    return {{"hz", hz}, {"midiNote", midiNote}, {"cents", cents}, {"confidence", confidence}, {"noteName", noteName}};
}

RootDetection detectRoot(const float* x, int64_t n, double sr) {
    RootDetection r;
    if (!x || n <= 0 || sr <= 0) return r;
    // settled pitch: windows after the pitch envelope (808s start sharp and drop)
    const double starts[3] = {0.08, 0.16, 0.26};
    std::vector<double> est;
    for (double s0 : starts) {
        const int64_t a = static_cast<int64_t>(s0 * sr);
        if (a + static_cast<int64_t>(0.05 * sr) >= n) break;
        const double f = lowFundamental(x + a, n - a, sr, 0.0);
        if (f > 0) est.push_back(f);
    }
    if (est.empty()) { // very short one-shot: whatever is there
        const double f = lowFundamental(x, n, sr, 0.01);
        if (f <= 0) return r;
        est.push_back(f);
    }
    const double hz = est.back(); // the latest window is the most settled
    double spread = 0;
    for (double f : est) spread = std::max(spread, std::fabs(1200.0 * std::log2(f / hz)));
    r.hz = hz;
    const double m = 69.0 + 12.0 * std::log2(hz / 440.0);
    r.midiNote = static_cast<int>(std::lround(m));
    r.cents = (m - r.midiNote) * 100.0;
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    r.noteName = std::format("{}{}", names[((r.midiNote % 12) + 12) % 12], r.midiNote / 12 - 1);
    // tonal share: energy near the fundamental (and 2nd harmonic) vs the whole < 1 kHz band
    const int N = 16384;
    std::vector<float> seg(N, 0.0f);
    const int64_t a = std::min<int64_t>(n - 1, static_cast<int64_t>(starts[0] * sr));
    for (int i = 0; i < N && a + i < n; ++i) seg[static_cast<size_t>(i)] = x[a + i];
    auto mag = dsp::magnitudeSpectrum(seg.data(), N);
    double tonal = 0, total = 0;
    for (size_t b = 1; b < mag.size() && b * sr / N < 1000.0; ++b) {
        const double f = b * sr / N, e = static_cast<double>(mag[b]) * mag[b];
        total += e;
        for (int h = 1; h <= 2; ++h)
            if (std::fabs(1200.0 * std::log2(f / (hz * h))) < 60.0) tonal += e;
    }
    const double share = total > 0 ? tonal / total : 0.0;
    r.confidence = std::clamp(share * (1.0 - std::min(1.0, spread / 100.0)), 0.0, 1.0);
    return r;
}

} // namespace roy::beat
