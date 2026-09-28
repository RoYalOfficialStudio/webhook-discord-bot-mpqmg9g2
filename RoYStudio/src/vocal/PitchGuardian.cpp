#include "vocal/PitchGuardian.h"
#include "core/Math.h"
#include "dsp/Resampler.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::vocal {

const char* guardianModeId(GuardianMode m) {
    switch (m) {
    case GuardianMode::Off: return "off";
    case GuardianMode::Warn: return "warn";
    case GuardianMode::Assist: return "assist";
    case GuardianMode::Lock: return "lock";
    }
    return "off";
}

GuardianMode guardianModeFromId(const std::string& s) {
    if (s == "warn") return GuardianMode::Warn;
    if (s == "assist") return GuardianMode::Assist;
    if (s == "lock") return GuardianMode::Lock;
    return GuardianMode::Off;
}

bool CorrectionPlan::any() const {
    for (double v : shift)
        if (std::fabs(v) > 1e-3) return true;
    return false;
}

namespace {

struct Target {
    bool correct = false;
    double midi = 0;
    std::string reason;
};

// Decides the target pitch for a sung pitch `m` according to the settings.
Target chooseTarget(double m, const PitchGuardianSettings& s) {
    Target t;
    const int semi = static_cast<int>(std::lround(m));
    const double devSemi = (m - semi) * 100.0;
    const bool semiInKey = s.key.contains(semi);
    const bool lock = s.mode == GuardianMode::Lock || s.scaleLock;
    if (lock) {
        if (s.allowChromatic && !semiInKey && std::fabs(devSemi) <= s.chromaticToleranceCents) {
            t.midi = semi;
            t.reason = "chromatic note kept (ALLOW CHROMATIC)";
        } else {
            t.midi = s.key.nearestPitch(m);
            t.reason = "locked to " + s.key.name();
        }
        t.correct = true;
        return t;
    }
    // ASSIST
    if (!semiInKey && s.offKeyFilter && !s.allowChromatic) {
        t.midi = s.key.nearestPitch(m);
        t.reason = std::format("off-key filter: {} is outside {}, target {}", noteName(semi), s.key.name(),
                               noteName(static_cast<int>(std::lround(t.midi))));
        t.correct = std::fabs(m - t.midi) * 100.0 > s.assistThresholdCents;
        return t;
    }
    t.midi = semi;
    if (std::fabs(devSemi) <= s.assistThresholdCents) {
        t.correct = false;
        t.reason = std::format("within +-{:.0f} cents - left natural", s.assistThresholdCents);
    } else {
        t.correct = true;
        t.reason = std::format("{:+.0f} cents from {}", devSemi, noteName(semi));
    }
    return t;
}

} // namespace

CorrectionPlan planCorrection(const PitchTrack& track, const PitchAnalysis& a, const PitchGuardianSettings& s) {
    CorrectionPlan plan;
    plan.hopSeconds = track.hopSeconds();
    const size_t n = track.frames.size();
    plan.shift.assign(n, 0.0);
    plan.warnings = tuningIssues(a, s.key, std::max(10.0, s.assistThresholdCents));
    if (s.mode == GuardianMode::Off || s.mode == GuardianMode::Warn) return plan;

    const double strength = std::clamp(s.strength, 0.0, 1.0);
    std::vector<double> raw(n, 0.0);
    std::vector<bool> defined(n, false);
    std::vector<double> noteShift(a.notes.size(), 0.0);

    for (size_t ni = 0; ni < a.notes.size(); ++ni) {
        const auto& note = a.notes[ni];
        NoteCorrection nc;
        nc.note = ni;
        nc.fromMidi = note.medianMidi;
        if (note.kind == FrameKind::Transition || note.kind == FrameKind::Slide) {
            nc.reason = note.kind == FrameKind::Slide ? "slide" : "transition";
            plan.notes.push_back(nc);
            continue;
        }
        if (note.meanConfidence < s.minConfidence * 0.8) {
            nc.reason = "low pitch confidence - not corrected";
            plan.notes.push_back(nc);
            continue;
        }
        Target t = chooseTarget(note.medianMidi, s);
        nc.toMidi = t.midi;
        nc.reason = t.reason;
        if (!t.correct) {
            plan.notes.push_back(nc);
            continue;
        }
        nc.corrected = true;
        nc.shift = (t.midi - note.medianMidi) * strength;
        noteShift[ni] = nc.shift;
        plan.notes.push_back(nc);
        const double h = std::clamp(s.humanize, 0.0, 1.0);
        const double vp = std::clamp(s.vibratoPreserve, 0.0, 1.0);
        for (size_t k = note.firstFrame; k <= note.lastFrame && k < n; ++k) {
            const auto& f = track.frames[k];
            if (!f.voiced) continue;
            const double dev = f.midi - note.medianMidi; // micro variation / vibrato around the centre
            const double keep = note.kind == FrameKind::Vibrato ? vp : h;
            raw[k] = nc.shift - strength * (1.0 - keep) * dev;
            defined[k] = true;
        }
    }

    // Slides, transitions and uncorrected gaps: interpolate between neighbouring note shifts,
    // blended with per-frame snapping by (1 - slidePreserve) for slides.
    auto neighbourShift = [&](size_t k, int dir) -> std::pair<double, bool> {
        for (long long j = static_cast<long long>(k) + dir; j >= 0 && j < static_cast<long long>(n); j += dir) {
            if (!track.frames[static_cast<size_t>(j)].voiced) return {0.0, false};
            if (defined[static_cast<size_t>(j)]) return {raw[static_cast<size_t>(j)], true};
            const int ni = a.noteOfFrame[static_cast<size_t>(j)];
            if (ni >= 0 && a.notes[static_cast<size_t>(ni)].kind != FrameKind::Transition &&
                a.notes[static_cast<size_t>(ni)].kind != FrameKind::Slide)
                return {noteShift[static_cast<size_t>(ni)], true};
        }
        return {0.0, false};
    };
    for (size_t k = 0; k < n; ++k) {
        if (defined[k] || !track.frames[k].voiced) continue;
        const int ni = a.noteOfFrame[k];
        const bool isSlide = ni >= 0 && a.notes[static_cast<size_t>(ni)].kind == FrameKind::Slide;
        const bool isTransition = ni >= 0 && a.notes[static_cast<size_t>(ni)].kind == FrameKind::Transition;
        if (!isSlide && !isTransition) continue;
        auto [prev, hasPrev] = neighbourShift(k, -1);
        auto [next, hasNext] = neighbourShift(k, +1);
        double interp = 0.0;
        if (hasPrev && hasNext) {
            const auto& note = a.notes[static_cast<size_t>(ni)];
            const double span = std::max<double>(1.0, static_cast<double>(note.lastFrame - note.firstFrame) + 1.0);
            const double x = (static_cast<double>(k - note.firstFrame) + 0.5) / span;
            interp = prev + (next - prev) * x;
        } else if (hasPrev) {
            interp = prev;
        } else if (hasNext) {
            interp = next;
        }
        if (isSlide) {
            const double sp = std::clamp(s.slidePreserve, 0.0, 1.0);
            const Target t = chooseTarget(track.frames[k].midi, s);
            const double snap = t.correct ? strength * (t.midi - track.frames[k].midi) : 0.0;
            raw[k] = sp * interp + (1.0 - sp) * snap;
        } else {
            raw[k] = interp;
        }
        defined[k] = true;
    }

    // Transition zones inside notes (onsets/offsets) and low-confidence frames.
    for (size_t k = 0; k < n; ++k) {
        if (!track.frames[k].voiced) {
            raw[k] = 0.0;
            continue;
        }
        const double conf = track.frames[k].confidence;
        if (conf < s.minConfidence) {
            const double w = std::clamp((conf - 0.5) / std::max(1e-6, s.minConfidence - 0.5), 0.0, 1.0);
            raw[k] *= w;
        }
    }

    // Retune speed: causal one-pole per voiced run, starting at the first target.
    const double hop = plan.hopSeconds;
    const double coef = s.speedMs <= 0 ? 0.0 : std::exp(-hop / (s.speedMs / 1000.0));
    bool inRun = false;
    double y = 0;
    for (size_t k = 0; k < n; ++k) {
        if (!track.frames[k].voiced) {
            inRun = false;
            plan.shift[k] = 0;
            continue;
        }
        if (!inRun) {
            y = raw[k];
            inRun = true;
        } else {
            y = raw[k] + (y - raw[k]) * coef;
        }
        plan.shift[k] = y;
    }
    return plan;
}

// ------------------------------------------------------------------ PSOLA
std::vector<std::vector<float>> psolaShift(const std::vector<std::vector<float>>& audio, double sr, const PitchTrack& track,
                                           const std::vector<double>& shift, bool formantPreserve) {
    std::vector<std::vector<float>> out = audio;
    if (audio.empty() || audio[0].empty() || track.frames.empty()) return out;
    const size_t nCh = audio.size();
    const int64_t len = static_cast<int64_t>(audio[0].size());
    std::vector<float> guide(static_cast<size_t>(len));
    for (size_t c = 0; c < nCh; ++c)
        for (int64_t i = 0; i < len; ++i) guide[static_cast<size_t>(i)] += audio[c][static_cast<size_t>(i)] / static_cast<float>(nCh);

    const double hop = track.hopSeconds();
    auto frameAt = [&](int64_t sample) {
        const double idx = static_cast<double>(sample) / sr / hop;
        return static_cast<size_t>(std::clamp<long long>(std::llround(idx), 0, static_cast<long long>(track.frames.size() - 1)));
    };
    auto shiftAt = [&](int64_t sample) {
        const double idx = static_cast<double>(sample) / sr / hop;
        const size_t i = static_cast<size_t>(std::clamp(idx, 0.0, static_cast<double>(shift.size() - 1)));
        const size_t j = std::min(i + 1, shift.size() - 1);
        const double f = std::clamp(idx - static_cast<double>(i), 0.0, 1.0);
        return shift[i] + (shift[j] - shift[i]) * f;
    };
    auto periodAt = [&](int64_t sample) {
        const double m = track.midiAt(static_cast<double>(sample) / sr);
        return m > 0 ? sr / midiToHz(m) : 0.0;
    };

    // Regions that need processing: voiced frames with non-zero shift (expanded 20 ms).
    std::vector<float> mask(static_cast<size_t>(len), 0.0f);
    const int64_t hopS = static_cast<int64_t>(hop * sr);
    const int64_t expand = static_cast<int64_t>(0.02 * sr);
    bool anyWork = false;
    for (size_t k = 0; k < track.frames.size() && k < shift.size(); ++k) {
        if (!track.frames[k].voiced || std::fabs(shift[k]) < 0.005) continue;
        anyWork = true;
        const int64_t c = static_cast<int64_t>(track.frames[k].time * sr);
        for (int64_t i = std::max<int64_t>(0, c - hopS / 2 - expand); i < std::min(len, c + hopS / 2 + expand); ++i) mask[static_cast<size_t>(i)] = 1.0f;
    }
    if (!anyWork) return out;
    // restrict the mask to voiced samples (PSOLA only works on periodic material)
    for (int64_t i = 0; i < len; ++i)
        if (mask[static_cast<size_t>(i)] > 0 && !track.frames[frameAt(i)].voiced) mask[static_cast<size_t>(i)] = 0.0f;
    // smooth mask edges (10 ms linear ramps) -> crossfade between original and processed
    {
        const int64_t ramp = static_cast<int64_t>(0.01 * sr);
        std::vector<float> m2 = mask;
        float level = 0;
        for (int64_t i = 0; i < len; ++i) {
            level = mask[static_cast<size_t>(i)] > 0 ? std::min(1.0f, level + 1.0f / ramp) : std::max(0.0f, level - 1.0f / ramp);
            m2[static_cast<size_t>(i)] = level;
        }
        level = 0;
        for (int64_t i = len - 1; i >= 0; --i) {
            level = mask[static_cast<size_t>(i)] > 0 ? std::min(1.0f, level + 1.0f / ramp) : std::max(0.0f, level - 1.0f / ramp);
            m2[static_cast<size_t>(i)] = std::min(m2[static_cast<size_t>(i)], level);
        }
        mask = std::move(m2);
    }

    // Voiced runs (in samples) that intersect the mask.
    std::vector<std::pair<int64_t, int64_t>> runs;
    for (int64_t i = 0; i < len;) {
        if (mask[static_cast<size_t>(i)] <= 0) { ++i; continue; }
        int64_t a = i;
        // extend to the whole voiced run for stable pitch marks
        while (a > 0 && track.frames[frameAt(a - 1)].voiced) --a;
        int64_t b = i;
        while (b < len && track.frames[frameAt(b)].voiced) ++b;
        runs.push_back({a, b});
        i = std::max(b, i + 1);
    }

    std::vector<std::vector<double>> acc(nCh, std::vector<double>(static_cast<size_t>(len), 0.0));
    std::vector<double> wsum(static_cast<size_t>(len), 0.0);
    std::vector<float> grain;
    for (auto [a, b] : runs) {
        // analysis pitch marks
        std::vector<int64_t> marks;
        double T0 = periodAt(a);
        if (T0 <= 2) continue;
        auto argmax = [&](int64_t s0, int64_t s1) {
            s0 = std::clamp<int64_t>(s0, a, b - 1);
            s1 = std::clamp<int64_t>(s1, s0 + 1, b);
            int64_t best = s0;
            for (int64_t i = s0; i < s1; ++i)
                if (guide[static_cast<size_t>(i)] > guide[static_cast<size_t>(best)]) best = i;
            return best;
        };
        int64_t m = argmax(a, a + static_cast<int64_t>(T0));
        while (m < b) {
            marks.push_back(m);
            double T = periodAt(m);
            if (T <= 2) T = T0;
            const int64_t pred = m + static_cast<int64_t>(T);
            if (pred >= b) break;
            int64_t nm = argmax(pred - static_cast<int64_t>(T * 0.2), pred + static_cast<int64_t>(T * 0.2) + 1);
            if (nm <= m) nm = m + std::max<int64_t>(1, static_cast<int64_t>(T));
            m = nm;
        }
        if (marks.size() < 2) continue;
        // synthesis
        double ts = static_cast<double>(marks.front());
        size_t k = 0;
        while (ts < static_cast<double>(b)) {
            while (k + 1 < marks.size() && std::fabs(static_cast<double>(marks[k + 1]) - ts) < std::fabs(static_cast<double>(marks[k]) - ts)) ++k;
            const int64_t mk = marks[k];
            const double T = k + 1 < marks.size() ? static_cast<double>(marks[k + 1] - mk)
                                                  : static_cast<double>(mk - marks[k - 1]);
            const double r = std::pow(2.0, shiftAt(static_cast<int64_t>(ts)) / 12.0);
            const int half = std::max(2, static_cast<int>(T));
            const int glen = 2 * half;
            const double centre = ts;
            for (size_t c = 0; c < nCh; ++c) {
                grain.assign(static_cast<size_t>(glen), 0.0f);
                for (int j = 0; j < glen; ++j) {
                    const int64_t src = mk - half + j;
                    const float w = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * j / glen));
                    grain[static_cast<size_t>(j)] = (src >= 0 && src < len ? audio[c][static_cast<size_t>(src)] : 0.0f) * w;
                }
                std::vector<float> g2;
                const std::vector<float>* g = &grain;
                int gl = glen;
                if (!formantPreserve && std::fabs(r - 1.0) > 1e-4) {
                    // resample the grain: formants move together with the pitch
                    g2 = dsp::resample(grain, r, 1.0, 8);
                    g = &g2;
                    gl = static_cast<int>(g2.size());
                }
                const int64_t startOut = static_cast<int64_t>(std::llround(centre)) - gl / 2;
                for (int j = 0; j < gl; ++j) {
                    const int64_t dst = startOut + j;
                    if (dst < 0 || dst >= len) continue;
                    acc[c][static_cast<size_t>(dst)] += (*g)[static_cast<size_t>(j)];
                }
                if (c == 0) {
                    for (int j = 0; j < gl; ++j) {
                        const int64_t dst = startOut + j;
                        if (dst < 0 || dst >= len) continue;
                        wsum[static_cast<size_t>(dst)] += 0.5 - 0.5 * std::cos(kTwoPi * j / gl);
                    }
                }
            }
            ts += T / r;
        }
    }
    for (size_t c = 0; c < nCh; ++c)
        for (int64_t i = 0; i < len; ++i) {
            const float mk = mask[static_cast<size_t>(i)];
            if (mk <= 0) continue;
            const double w = wsum[static_cast<size_t>(i)];
            if (w < 0.2) continue; // no grain coverage: keep original
            const float processed = static_cast<float>(acc[c][static_cast<size_t>(i)] / w);
            out[c][static_cast<size_t>(i)] = mk * processed + (1.0f - mk) * audio[c][static_cast<size_t>(i)];
        }
    return out;
}

std::vector<std::vector<float>> applyCorrection(const std::vector<std::vector<float>>& audio, double sr, const PitchTrack& track,
                                                const CorrectionPlan& plan, bool formantPreserve) {
    return psolaShift(audio, sr, track, plan.shift, formantPreserve);
}

GuardianResult runPitchGuardian(const std::vector<std::vector<float>>& audio, double sr, const PitchGuardianSettings& s) {
    GuardianResult r;
    r.track = detectPitch(audio, sr);
    r.analysis = analyzePitch(r.track);
    r.plan = planCorrection(r.track, r.analysis, s);
    r.audio = r.plan.any() ? applyCorrection(audio, sr, r.track, r.plan, s.formantPreserve) : audio;
    return r;
}

} // namespace roy::vocal
