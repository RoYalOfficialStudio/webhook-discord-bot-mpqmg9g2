#include "vocal/VocalTools.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "dsp/TimeStretch.h"
#include "vocal/PitchGuardian.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roy::vocal {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<double> movingAverage(const std::vector<double>& v, int half) {
    std::vector<double> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        double s = 0;
        int c = 0;
        for (long k = static_cast<long>(i) - half; k <= static_cast<long>(i) + half; ++k)
            if (k >= 0 && k < static_cast<long>(v.size()) && std::isfinite(v[static_cast<size_t>(k)])) {
                s += v[static_cast<size_t>(k)];
                ++c;
            }
        out[i] = c ? s / c : 0.0;
    }
    return out;
}

std::vector<double> medianFilter(const std::vector<double>& v, int half) {
    std::vector<double> out(v.size());
    std::vector<double> w;
    for (size_t i = 0; i < v.size(); ++i) {
        w.clear();
        for (long k = static_cast<long>(i) - half; k <= static_cast<long>(i) + half; ++k)
            if (k >= 0 && k < static_cast<long>(v.size())) w.push_back(v[static_cast<size_t>(k)]);
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
        out[i] = w[w.size() / 2];
    }
    return out;
}

std::vector<bool> activity(const std::vector<float>& x, double sr, int hop) {
    auto fl = dsp::frameLevels(x.data(), static_cast<int64_t>(x.size()), sr, 2.0 * hop / sr, static_cast<double>(hop) / sr);
    const double floor = dsp::percentile(fl.rmsDb, 0.1);
    const double peak = dsp::percentile(fl.rmsDb, 0.98);
    const double thr = std::max(floor + 12.0, peak - 35.0);
    std::vector<bool> a(fl.rmsDb.size());
    for (size_t i = 0; i < a.size(); ++i) a[i] = fl.rmsDb[i] > thr;
    return a;
}
} // namespace

// ---------------------------------------------------------------- microscope
MicroscopeReport inspectRegion(const Channels& audio, double sr, double t0, double t1) {
    MicroscopeReport r;
    if (audio.empty() || audio[0].empty()) return r;
    const auto mono = dsp::mixToMono(audio);
    const int64_t n = static_cast<int64_t>(mono.size());
    const int64_t a = std::clamp<int64_t>(static_cast<int64_t>(t0 * sr), 0, n);
    const int64_t b = std::clamp<int64_t>(static_cast<int64_t>(t1 * sr), a, n);
    r.start = static_cast<double>(a) / sr;
    r.end = static_cast<double>(b) / sr;
    if (b - a < 32) return r;
    const float* x = mono.data() + a;
    const int64_t len = b - a;
    double sq = 0;
    float pk = 0;
    for (int64_t i = 0; i < len; ++i) {
        sq += static_cast<double>(x[i]) * x[i];
        pk = std::max(pk, std::fabs(x[i]));
    }
    r.peakDb = gainToDb(static_cast<double>(pk));
    r.rmsDb = gainToDb(std::sqrt(sq / static_cast<double>(len)));
    auto fl = dsp::frameLevels(x, len, sr, 0.01, 0.005);
    r.noiseFloorDb = dsp::percentile(fl.rmsDb, 0.05);

    auto track = detectPitch(x, len, sr);
    std::vector<double> v;
    for (auto& f : track.frames)
        if (f.voiced) v.push_back(f.midi);
    r.voicedRatio = track.frames.empty() ? 0 : static_cast<double>(v.size()) / static_cast<double>(track.frames.size());
    if (!v.empty()) {
        std::sort(v.begin(), v.end());
        r.pitchMedianMidi = v[v.size() / 2];
        r.pitchMinMidi = v.front();
        r.pitchMaxMidi = v.back();
        double mean = 0, var = 0;
        for (double q : v) mean += q;
        mean /= static_cast<double>(v.size());
        for (double q : v) var += (q - mean) * (q - mean);
        r.pitchStabilityCents = std::sqrt(var / static_cast<double>(v.size())) * 100.0;
        // formants from the voiced portion
        std::vector<float> voiced;
        for (auto& f : track.frames) {
            if (!f.voiced) continue;
            const int64_t c = static_cast<int64_t>(f.time * sr);
            for (int64_t i = std::max<int64_t>(0, c - track.hop / 2); i < std::min(len, c + track.hop / 2); ++i) voiced.push_back(x[i]);
        }
        r.formantsHz = dsp::estimateFormants(voiced.data(), static_cast<int64_t>(voiced.size()), sr, 3);
    }
    dsp::OnsetSettings os;
    os.hop = 128;
    os.fftSize = 512;
    for (auto& o : dsp::detectOnsets(x, len, sr, os)) r.onsets.push_back(o.time + r.start);
    auto eAll = dsp::bandEnergy(x, len, sr, 20, sr / 2, 1024, 512);
    auto eSib = dsp::bandEnergy(x, len, sr, 5000, std::min(10000.0, sr / 2), 1024, 512);
    double tAll = 0, tSib = 0;
    for (size_t i = 0; i < eAll.size(); ++i) {
        tAll += eAll[i];
        tSib += eSib[i];
    }
    r.sibilanceRatioDb = 10 * std::log10((tSib + 1e-20) / (tAll + 1e-20));
    r.looksLikeBreath = r.voicedRatio < 0.1 && r.rmsDb < -22.0 && r.rmsDb > -55.0 && r.end - r.start >= 0.12;
    // attack time of the first onset: 10 % -> 90 % of the local peak envelope
    const int64_t env = std::max<int64_t>(1, static_cast<int64_t>(sr * 0.0005));
    const int64_t oStart = r.onsets.empty() ? 0 : static_cast<int64_t>((r.onsets[0] - r.start) * sr);
    const int64_t oEnd = std::min(len, oStart + static_cast<int64_t>(0.1 * sr));
    std::vector<float> e;
    for (int64_t i = std::max<int64_t>(0, oStart - env * 4); i < oEnd; i += env) {
        float m = 0;
        for (int64_t k = i; k < std::min(oEnd, i + env); ++k) m = std::max(m, std::fabs(x[k]));
        e.push_back(m);
    }
    if (!e.empty()) {
        const float peakE = *std::max_element(e.begin(), e.end());
        int i10 = -1, i90 = -1;
        for (size_t k = 0; k < e.size(); ++k) {
            if (i10 < 0 && e[k] >= 0.1f * peakE) i10 = static_cast<int>(k);
            if (i90 < 0 && e[k] >= 0.9f * peakE) i90 = static_cast<int>(k);
        }
        if (i10 >= 0 && i90 >= i10) r.attackMs = static_cast<double>((i90 - i10) * env) / sr * 1000.0;
    }
    return r;
}

Channels applyRegionGain(const Channels& audio, double sr, double t0, double t1, double gainDb, double fadeMs) {
    Channels out = audio;
    const double g = dbToGain(gainDb);
    const double fade = std::max(1.0, fadeMs / 1000.0 * sr);
    for (auto& ch : out) {
        const int64_t n = static_cast<int64_t>(ch.size());
        const int64_t a = std::clamp<int64_t>(static_cast<int64_t>(t0 * sr), 0, n);
        const int64_t b = std::clamp<int64_t>(static_cast<int64_t>(t1 * sr), a, n);
        for (int64_t i = std::max<int64_t>(0, a - static_cast<int64_t>(fade)); i < std::min(n, b + static_cast<int64_t>(fade)); ++i) {
            double w = 1.0;
            if (i < a) w = 1.0 - static_cast<double>(a - i) / fade;
            else if (i >= b) w = 1.0 - static_cast<double>(i - b + 1) / fade;
            w = std::clamp(w, 0.0, 1.0);
            ch[static_cast<size_t>(i)] = static_cast<float>(ch[static_cast<size_t>(i)] * (1.0 + (g - 1.0) * w));
        }
    }
    return out;
}

Channels applyRegionPitchShift(const Channels& audio, double sr, double t0, double t1, double semis, bool formant) {
    auto track = detectPitch(audio, sr);
    std::vector<double> shift(track.frames.size(), 0.0);
    const double ramp = 0.01;
    for (size_t i = 0; i < shift.size(); ++i) {
        const double t = track.frames[i].time;
        double w = 0;
        if (t >= t0 && t < t1) w = 1;
        else if (t >= t0 - ramp && t < t0) w = (t - (t0 - ramp)) / ramp;
        else if (t >= t1 && t < t1 + ramp) w = 1 - (t - t1) / ramp;
        shift[i] = semis * std::clamp(w, 0.0, 1.0);
    }
    return psolaShift(audio, sr, track, shift, formant);
}

// ---------------------------------------------------------------- double magnet
const char* magnetModeId(MagnetMode m) {
    switch (m) {
    case MagnetMode::Loose: return "loose";
    case MagnetMode::Natural: return "natural";
    case MagnetMode::Tight: return "tight";
    case MagnetMode::UltraTight: return "ultra_tight";
    }
    return "natural";
}

MagnetMode magnetModeFromId(const std::string& s) {
    if (s == "loose") return MagnetMode::Loose;
    if (s == "tight") return MagnetMode::Tight;
    if (s == "ultra_tight" || s == "ultra") return MagnetMode::UltraTight;
    return MagnetMode::Natural;
}

std::vector<double> measureOffsets(const Channels& a, const Channels& b, double sr, double maxOffsetMs, double hopSec,
                                   std::vector<bool>* activeMask) {
    const int hop = std::max(16, static_cast<int>(hopSec * sr));
    const auto ma = dsp::mixToMono(a), mb = dsp::mixToMono(b);
    auto fa = dsp::alignmentFeatures(ma.data(), static_cast<int64_t>(ma.size()), sr, hop);
    auto fb = dsp::alignmentFeatures(mb.data(), static_cast<int64_t>(mb.size()), sr, hop);
    const int band = static_cast<int>(maxOffsetMs / 1000.0 * sr / hop) + 2;
    auto path = dsp::dtwAlign(fa, fb, band);
    std::vector<double> sumI(fb.size(), 0.0);
    std::vector<int> cnt(fb.size(), 0);
    for (auto [i, j] : path) {
        sumI[static_cast<size_t>(j)] += i;
        cnt[static_cast<size_t>(j)]++;
    }
    std::vector<double> off(fb.size(), 0.0);
    for (size_t j = 0; j < fb.size(); ++j)
        off[j] = cnt[j] ? (static_cast<double>(j) - sumI[j] / cnt[j]) * hop / sr * 1000.0 : 0.0;
    off = movingAverage(medianFilter(off, 5), 3);
    if (activeMask) {
        auto actA = activity(ma, sr, hop), actB = activity(mb, sr, hop);
        activeMask->assign(off.size(), false);
        for (size_t j = 0; j < off.size(); ++j) {
            const long i = static_cast<long>(std::lround(static_cast<double>(j) - off[j] / 1000.0 * sr / hop));
            (*activeMask)[j] = j < actB.size() && actB[j] && i >= 0 && static_cast<size_t>(i) < actA.size() && actA[static_cast<size_t>(i)];
        }
    }
    return off;
}

namespace {
void offsetStats(const std::vector<double>& off, const std::vector<bool>& mask, double& meanAbs, double& maxAbs, double& within20) {
    double s = 0, mx = 0;
    int c = 0, w = 0;
    for (size_t i = 0; i < off.size(); ++i) {
        if (!mask[i]) continue;
        s += std::fabs(off[i]);
        mx = std::max(mx, std::fabs(off[i]));
        if (std::fabs(off[i]) <= 20.0) ++w;
        ++c;
    }
    meanAbs = c ? s / c : 0;
    maxAbs = mx;
    within20 = c ? static_cast<double>(w) / c : 0;
}

double meanPitchDiff(const Channels& a, const Channels& b, double sr, double maxDiff) {
    auto pa = detectPitch(a, sr), pb = detectPitch(b, sr);
    double s = 0;
    int c = 0;
    for (size_t i = 0; i < std::min(pa.frames.size(), pb.frames.size()); ++i) {
        if (!pa.frames[i].voiced || !pb.frames[i].voiced) continue;
        const double d = pb.frames[i].midi - pa.frames[i].midi;
        if (std::fabs(d) > maxDiff) continue;
        s += std::fabs(d) * 100.0;
        ++c;
    }
    return c ? s / c : 0.0;
}
} // namespace

MagnetResult alignDouble(const Channels& mainV, const Channels& dbl, double sr, const MagnetSettings& s) {
    MagnetResult res;
    res.hopSeconds = 0.005;
    res.audio = dbl;
    if (mainV.empty() || dbl.empty() || dbl[0].empty()) return res;
    std::vector<bool> mask;
    auto off = measureOffsets(mainV, dbl, sr, s.maxOffsetMs, res.hopSeconds, &mask);
    res.offsetMs = off;
    double dummy;
    offsetStats(off, mask, res.report.meanAbsOffsetBeforeMs, res.report.maxOffsetBeforeMs, res.report.within20msBefore);
    res.report.meanPitchDiffBeforeCents = s.alignPitch ? meanPitchDiff(mainV, dbl, sr, s.maxPitchDiffSemitones) : 0.0;

    if (s.alignTiming) {
        double strength = 0.75, dead = 15.0;
        switch (s.mode) {
        case MagnetMode::Loose: strength = 0.5; dead = 30.0; break;
        case MagnetMode::Natural: strength = 0.75; dead = 15.0; break;
        case MagnetMode::Tight: strength = 0.9; dead = 5.0; break;
        case MagnetMode::UltraTight: strength = 1.0; dead = 0.0; break;
        }
        std::vector<double> applied(off.size(), kNaN);
        for (size_t j = 0; j < off.size(); ++j)
            if (mask[j]) {
                const double o = off[j];
                applied[j] = (o > 0 ? 1.0 : -1.0) * std::max(0.0, std::fabs(o) - dead) * strength;
            }
        // Inactive frames (silence between syllables): interpolate linearly between the
        // neighbouring corrections so timing changes happen in the gaps, not on the onsets.
        {
            long prevIdx = -1;
            for (long j = 0; j <= static_cast<long>(applied.size()); ++j) {
                const bool act = j < static_cast<long>(applied.size()) && std::isfinite(applied[static_cast<size_t>(j)]);
                if (!act) continue;
                if (j - prevIdx > 1) {
                    const double a = prevIdx >= 0 ? applied[static_cast<size_t>(prevIdx)] : applied[static_cast<size_t>(j)];
                    const double b = applied[static_cast<size_t>(j)];
                    for (long k = prevIdx + 1; k < j; ++k) {
                        const double f = prevIdx >= 0 ? static_cast<double>(k - prevIdx) / static_cast<double>(j - prevIdx) : 1.0;
                        applied[static_cast<size_t>(k)] = a + (b - a) * f;
                    }
                }
                prevIdx = j;
            }
            const double tail = prevIdx >= 0 ? applied[static_cast<size_t>(prevIdx)] : 0.0;
            for (size_t k = static_cast<size_t>(prevIdx + 1); k < applied.size(); ++k) applied[k] = tail;
        }
        // Within a syllable use the median correction (one offset per syllable keeps it natural).
        {
            size_t k = 0;
            while (k < mask.size()) {
                if (!mask[k]) { ++k; continue; }
                size_t e = k;
                while (e < mask.size() && mask[e]) ++e;
                std::vector<double> v(applied.begin() + static_cast<long>(k), applied.begin() + static_cast<long>(e));
                std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
                const double med = v[v.size() / 2];
                // hold the syllable's value a little before its onset as well
                const size_t pre = k >= 4 ? k - 4 : 0;
                for (size_t q = pre; q < e; ++q) applied[q] = med;
                k = e;
            }
        }
        applied = movingAverage(applied, 2);
        // mapping: input frame time t_j -> output time u_j = t_j - applied_j
        const double hop = res.hopSeconds;
        std::vector<double> u(applied.size());
        for (size_t j = 0; j < u.size(); ++j) {
            u[j] = static_cast<double>(j) * hop - applied[j] / 1000.0;
            if (j > 0) u[j] = std::max(u[j], u[j - 1] + hop * 0.3);
        }
        const int64_t outLen = static_cast<int64_t>(dbl[0].size());
        auto inputFor = [&](int64_t o) {
            const double t = static_cast<double>(o) / sr;
            auto it = std::lower_bound(u.begin(), u.end(), t);
            if (it == u.begin()) return (t - (u.front() - 0.0)) * sr + 0.0;
            if (it == u.end()) return (static_cast<double>(u.size() - 1) * hop + (t - u.back())) * sr;
            const size_t k = static_cast<size_t>(it - u.begin());
            const double f = (t - u[k - 1]) / std::max(1e-12, u[k] - u[k - 1]);
            return ((static_cast<double>(k - 1) + f) * hop) * sr;
        };
        res.audio = dsp::timeWarp(dbl, outLen, inputFor, sr);
    }
    if (s.alignPitch) {
        auto pm = detectPitch(mainV, sr);
        auto pd = detectPitch(res.audio, sr);
        std::vector<double> shift(pd.frames.size(), 0.0);
        for (size_t k = 0; k < pd.frames.size(); ++k) {
            if (!pd.frames[k].voiced) continue;
            const double target = pm.midiAt(pd.frames[k].time);
            if (target <= 0) continue;
            const double d = target - pd.frames[k].midi;
            if (std::fabs(d) <= s.maxPitchDiffSemitones) shift[k] = d * std::clamp(s.pitchStrength, 0.0, 1.0);
        }
        shift = movingAverage(shift, 4);
        res.audio = psolaShift(res.audio, sr, pd, shift, true);
        res.report.meanPitchDiffAfterCents = meanPitchDiff(mainV, res.audio, sr, s.maxPitchDiffSemitones);
    }
    std::vector<bool> mask2;
    auto off2 = measureOffsets(mainV, res.audio, sr, s.maxOffsetMs, res.hopSeconds, &mask2);
    offsetStats(off2, mask2, res.report.meanAbsOffsetAfterMs, dummy, res.report.within20msAfter);
    return res;
}

// ---------------------------------------------------------------- ghost take
GhostComparison compareTakes(const Channels& prev, const Channels& cur, double sr) {
    GhostComparison g;
    g.hopSeconds = 0.005;
    if (prev.empty() || cur.empty()) return g;
    std::vector<bool> mask;
    auto off = measureOffsets(prev, cur, sr, 300.0, g.hopSeconds, &mask);
    g.timingOffsetMs.assign(off.size(), kNaN);
    double s = 0;
    int c = 0;
    for (size_t j = 0; j < off.size(); ++j)
        if (mask[j]) {
            g.timingOffsetMs[j] = off[j];
            s += std::fabs(off[j]);
            ++c;
        }
    g.meanAbsTimingMs = c ? s / c : 0;
    auto pp = detectPitch(prev, sr), pc = detectPitch(cur, sr);
    g.pitchDiffCents.assign(off.size(), kNaN);
    double ps = 0;
    int pcnt = 0;
    for (size_t j = 0; j < off.size(); ++j) {
        const double t = static_cast<double>(j) * g.hopSeconds;
        const double mc = pc.midiAt(t), mp = pp.midiAt(t - off[j] / 1000.0);
        if (mc > 0 && mp > 0) {
            g.pitchDiffCents[j] = (mc - mp) * 100.0;
            ps += std::fabs(g.pitchDiffCents[j]);
            ++pcnt;
        }
    }
    g.meanAbsPitchCents = pcnt ? ps / pcnt : 0;
    const auto mp = dsp::mixToMono(prev), mc = dsp::mixToMono(cur);
    const int64_t hop = static_cast<int64_t>(g.hopSeconds * sr);
    auto peaks = [&](const std::vector<float>& x) {
        std::vector<float> out;
        for (int64_t i = 0; i < static_cast<int64_t>(x.size()); i += hop) {
            float m = 0;
            for (int64_t k = i; k < std::min<int64_t>(static_cast<int64_t>(x.size()), i + hop); ++k) m = std::max(m, std::fabs(x[static_cast<size_t>(k)]));
            out.push_back(m);
        }
        return out;
    };
    g.previousPeaks = peaks(mp);
    g.currentPeaks = peaks(mc);
    return g;
}

} // namespace roy::vocal
