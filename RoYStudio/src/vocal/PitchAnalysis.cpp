#include "vocal/PitchAnalysis.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::vocal {

const char* frameKindName(FrameKind k) {
    switch (k) {
    case FrameKind::Unvoiced: return "unvoiced";
    case FrameKind::Uncertain: return "uncertain";
    case FrameKind::Stable: return "stable";
    case FrameKind::Vibrato: return "vibrato";
    case FrameKind::Slide: return "slide";
    case FrameKind::Transition: return "transition";
    }
    return "?";
}

namespace {
double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
}
} // namespace

PitchAnalysis analyzePitch(const PitchTrack& t, const PitchAnalysisSettings& s) {
    PitchAnalysis a;
    const size_t n = t.frames.size();
    a.kinds.assign(n, FrameKind::Unvoiced);
    a.noteOfFrame.assign(n, -1);
    if (n == 0) return a;
    const double hop = t.hopSeconds();

    // 3-point median smoothing of voiced pitch
    std::vector<double> p(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (!t.frames[i].voiced) continue;
        std::vector<double> w;
        for (size_t k = (i ? i - 1 : 0); k <= std::min(n - 1, i + 1); ++k)
            if (t.frames[k].voiced) w.push_back(t.frames[k].midi);
        p[i] = median(w);
    }

    // segment voiced runs into notes
    const size_t splitFrames = std::max<size_t>(2, static_cast<size_t>(s.splitMinSeconds / hop));
    const size_t medWin = std::max<size_t>(8, static_cast<size_t>(0.16 / hop));
    std::vector<std::pair<size_t, size_t>> segs;
    std::vector<bool> slideSeg;

    // Splits a steady span [b, e] with a running median (new note on a sustained jump).
    auto splitSteady = [&](size_t b, size_t e) {
        size_t start = b;
        std::vector<double> hist;
        size_t devRun = 0;
        for (size_t j = b; j <= e; ++j) {
            const double m = hist.empty() ? p[j] : median(std::vector<double>(hist.end() - static_cast<long>(std::min(hist.size(), medWin)), hist.end()));
            if (!hist.empty() && std::fabs(p[j] - m) > s.splitSemitones) {
                if (++devRun >= splitFrames) {
                    const size_t cut = j + 1 - devRun;
                    segs.push_back({start, cut - 1});
                    slideSeg.push_back(false);
                    start = cut;
                    hist.assign(p.begin() + static_cast<long>(cut), p.begin() + static_cast<long>(j) + 1);
                    devRun = 0;
                    continue;
                }
            } else {
                devRun = 0;
            }
            hist.push_back(p[j]);
        }
        segs.push_back({start, e});
        slideSeg.push_back(false);
    };

    // Legato: a sustained change of the mean pitch level (window >= one vibrato period)
    // splits a span; the glide between the two levels becomes its own slide segment.
    auto splitLevels = [&](size_t b, size_t e) {
        const size_t w = std::max<size_t>(4, static_cast<size_t>(0.22 / hop));
        if (e < b + 2 * w + 2) {
            splitSteady(b, e);
            return;
        }
        std::vector<double> pre(e - b + 2, 0.0);
        for (size_t k = b; k <= e; ++k) pre[k - b + 1] = pre[k - b] + p[k];
        auto mean = [&](size_t a, size_t z) { return (pre[z - b + 1] - pre[a - b]) / static_cast<double>(z - a + 1); };
        size_t cur = b;
        size_t j = b + w;
        while (j + w <= e) {
            const double l1 = mean(j - w, j - 1), l2 = mean(j, j + w - 1);
            if (std::fabs(l2 - l1) < 0.7) {
                ++j;
                continue;
            }
            // strongest change nearby
            size_t best = j;
            double bestD = std::fabs(l2 - l1);
            for (size_t q = j + 1; q + w <= e && q < j + w; ++q) {
                const double d = std::fabs(mean(q, q + w - 1) - mean(q - w, q - 1));
                if (d > bestD) {
                    bestD = d;
                    best = q;
                }
            }
            const double L1 = mean(best - w, best - 1), L2 = mean(best, best + w - 1);
            // transition region: frames not yet near either level
            size_t rs = best, re = best;
            while (rs > cur + 1 && std::fabs(p[rs - 1] - L1) > 0.3) --rs;
            while (re + 1 <= e && std::fabs(p[re + 1] - L2) > 0.3 && re + 1 < best + w) ++re;
            if (rs > cur) splitSteady(cur, rs - 1);
            if (static_cast<double>(re - rs + 1) * hop >= 0.04) {
                segs.push_back({rs, re});
                slideSeg.push_back(true);
                cur = re + 1;
            } else {
                cur = rs;
            }
            j = std::max(cur + w, re + 1);
        }
        if (cur <= e) splitSteady(cur, e);
    };

    const int slopeHalf = std::max(2, static_cast<int>(0.025 / hop));
    size_t i = 0;
    while (i < n) {
        if (!t.frames[i].voiced) { ++i; continue; }
        size_t runEnd = i;
        while (runEnd + 1 < n && t.frames[runEnd + 1].voiced) ++runEnd;
        // local slope (semitones / second) by regression over +-25 ms
        std::vector<double> slope(runEnd - i + 1, 0.0);
        for (size_t k = i; k <= runEnd; ++k) {
            const size_t lo = k >= i + static_cast<size_t>(slopeHalf) ? k - static_cast<size_t>(slopeHalf) : i;
            const size_t hi = std::min(runEnd, k + static_cast<size_t>(slopeHalf));
            double sx = 0, sy = 0, sxx = 0, sxy = 0, N = 0;
            for (size_t q = lo; q <= hi; ++q) {
                const double x = static_cast<double>(q) * hop, y = p[q];
                sx += x; sy += y; sxx += x * x; sxy += x * y; N += 1;
            }
            const double den = N * sxx - sx * sx;
            slope[k - i] = den > 1e-12 ? (N * sxy - sx * sy) / den : 0.0;
        }
        // candidate moving groups
        struct Group { size_t b, e; int dir; double change; };
        std::vector<Group> groups;
        for (size_t k = i; k <= runEnd;) {
            if (std::fabs(slope[k - i]) < 8.0) { ++k; continue; }
            const int dir = slope[k - i] > 0 ? 1 : -1;
            size_t e = k;
            while (e + 1 <= runEnd && slope[e + 1 - i] * dir >= 8.0) ++e;
            groups.push_back({k, e, dir, p[e] - p[k]});
            k = e + 1;
        }
        std::vector<std::pair<size_t, size_t>> slides;
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& G = groups[g];
            if (std::fabs(G.change) < 1.0 || static_cast<double>(G.e - G.b + 1) * hop < 0.04) continue;
            // A slide moves the pitch LEVEL; vibrato swings around an unchanged centre.
            // Compare the mean pitch over ~one vibrato period before and after the group.
            const size_t ctx = std::max<size_t>(3, static_cast<size_t>(0.2 / hop));
            auto meanRange = [&](size_t a, size_t b2) {
                double sum = 0, cnt = 0;
                for (size_t q = a; q <= b2; ++q) {
                    sum += p[q];
                    cnt += 1;
                }
                return cnt > 0 ? sum / cnt : 0.0;
            };
            const size_t bb = G.b >= i + ctx ? G.b - ctx : i;
            const size_t ae = std::min(runEnd, G.e + ctx);
            if (G.b == bb || G.e == ae) { // at a run edge: fall back to the size of the move
                if (std::fabs(G.change) >= 1.5) slides.push_back({G.b, G.e});
                continue;
            }
            const double before = meanRange(bb, G.b - 1), after = meanRange(G.e + 1, ae);
            if (std::fabs(after - before) >= 0.8 && (after - before) * G.dir > 0) slides.push_back({G.b, G.e});
        }
        size_t cur = i;
        for (auto [b, e] : slides) {
            if (b > cur) splitLevels(cur, b - 1);
            segs.push_back({b, e});
            slideSeg.push_back(true);
            cur = e + 1;
        }
        if (cur <= runEnd) splitLevels(cur, runEnd);
        i = runEnd + 1;
    }

    const size_t transFrames = std::max<size_t>(1, static_cast<size_t>(s.transitionSeconds / hop));
    double sustained = 0, noteSum = 0, inTune = 0, stableCount = 0;
    for (size_t si = 0; si < segs.size(); ++si) {
        const auto [b, e] = segs[si];
        NoteSegment ns;
        ns.firstFrame = b;
        ns.lastFrame = e;
        ns.start = t.frames[b].time - hop / 2;
        ns.end = t.frames[e].time + hop / 2;
        std::vector<double> vals(p.begin() + static_cast<long>(b), p.begin() + static_cast<long>(e) + 1);
        ns.medianMidi = median(vals);
        double conf = 0;
        for (size_t k = b; k <= e; ++k) conf += t.frames[k].confidence;
        ns.meanConfidence = conf / static_cast<double>(e - b + 1);
        ns.nearestNote = static_cast<int>(std::lround(ns.medianMidi));
        ns.centsFromNearest = (ns.medianMidi - ns.nearestNote) * 100.0;
        const double dur = ns.duration();
        const size_t len = e - b + 1;
        if (len <= transFrames) {
            ns.kind = FrameKind::Transition;
        } else {
            // linear regression pitch vs frame
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t k = 0; k < len; ++k) {
                const double x = static_cast<double>(k), y = vals[k];
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double N = static_cast<double>(len);
            const double slope = (N * sxy - sx * sy) / std::max(1e-12, N * sxx - sx * sx);
            const double icpt = (sy - slope * sx) / N;
            double ssRes = 0, ssTot = 0;
            const double mean = sy / N;
            std::vector<double> resid(len);
            for (size_t k = 0; k < len; ++k) {
                resid[k] = vals[k] - (icpt + slope * static_cast<double>(k));
                ssRes += resid[k] * resid[k];
                ssTot += (vals[k] - mean) * (vals[k] - mean);
            }
            const double r2 = ssTot > 1e-12 ? 1.0 - ssRes / ssTot : 0.0;
            const double change = slope * (N - 1);
            if (slideSeg[si] || (std::fabs(change) >= 1.0 && r2 > 0.8 && dur < 0.6)) {
                ns.kind = FrameKind::Slide;
                ns.slideSemitones = change;
            } else {
                // vibrato: oscillation of the residual around the centre
                int crossings = 0;
                double peakDev = 0;
                for (size_t k = 1; k < len; ++k) {
                    if ((resid[k - 1] < 0) != (resid[k] < 0)) ++crossings;
                    peakDev = std::max(peakDev, std::fabs(resid[k]));
                }
                const double rate = crossings / 2.0 / std::max(1e-9, dur);
                const double rmsDev = std::sqrt(ssRes / N);
                const double extent = rmsDev * std::sqrt(2.0) * 100.0;
                if (dur >= 0.25 && rate >= 3.5 && rate <= 9.0 && extent >= 15.0 && extent <= 200.0) {
                    ns.kind = FrameKind::Vibrato;
                    ns.vibratoRateHz = rate;
                    ns.vibratoExtentCents = extent;
                } else {
                    ns.kind = FrameKind::Stable;
                }
            }
        }
        const int idx = static_cast<int>(a.notes.size());
        for (size_t k = b; k <= e; ++k) {
            a.noteOfFrame[k] = idx;
            FrameKind fk = ns.kind;
            if (ns.kind != FrameKind::Transition && ns.kind != FrameKind::Slide && len > 3 * transFrames / 2) {
                // onset/offset of a note: short transition zones
                const size_t edge = std::max<size_t>(1, transFrames / 3);
                if (k < b + edge || k + edge > e) fk = FrameKind::Transition;
            }
            if (t.frames[k].confidence < s.uncertainConfidence) fk = FrameKind::Uncertain;
            a.kinds[k] = fk;
        }
        a.voicedSeconds += dur;
        noteSum += dur;
        if ((ns.kind == FrameKind::Stable || ns.kind == FrameKind::Vibrato) && dur >= 0.15) sustained += dur;
        if (ns.kind == FrameKind::Stable && dur >= 0.1) {
            stableCount += 1;
            if (std::fabs(ns.centsFromNearest) <= 25.0) inTune += 1;
        }
        a.notes.push_back(ns);
    }
    a.meanNoteSeconds = a.notes.empty() ? 0 : noteSum / static_cast<double>(a.notes.size());
    a.sustainedRatio = a.voicedSeconds > 0 ? sustained / a.voicedSeconds : 0;
    a.rapIndicator = 1.0 - a.sustainedRatio;
    a.inTuneRatio = stableCount > 0 ? inTune / stableCount : 0;
    return a;
}

std::vector<TuningIssue> tuningIssues(const PitchAnalysis& a, const Key& key, double thr) {
    std::vector<TuningIssue> out;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const auto& n = a.notes[i];
        if (n.kind != FrameKind::Stable && n.kind != FrameKind::Vibrato) continue;
        if (n.duration() < 0.08) continue;
        const bool offKey = !key.contains(n.nearestNote);
        const double target = offKey ? key.nearestPitch(n.medianMidi) : n.nearestNote;
        const double cents = (n.medianMidi - target) * 100.0;
        if (std::fabs(cents) > thr || offKey) {
            TuningIssue t;
            t.note = i;
            t.time = n.start;
            t.centsOff = cents;
            t.offKey = offKey;
            t.text = std::format("{:.2f}s: {} {:+.0f} cents{}", n.start, noteName(static_cast<int>(std::lround(target))), cents,
                                 offKey ? std::format(" (sung {} is outside {})", noteName(n.nearestNote), key.name()) : "");
            out.push_back(t);
        }
    }
    return out;
}

} // namespace roy::vocal
