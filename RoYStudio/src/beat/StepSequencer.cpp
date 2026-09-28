#include "beat/StepSequencer.h"
#include "core/Files.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace roy {

namespace beat {

void setPatternLength(Pattern& p, int numSteps) {
    numSteps = std::clamp(numSteps, 1, 256);
    for (auto& row : p.rows) {
        const size_t old = row.steps.size();
        row.steps.resize(static_cast<size_t>(numSteps));
        if (old > 0)
            for (size_t i = old; i < row.steps.size(); ++i) row.steps[i] = row.steps[i % old];
    }
    p.numSteps = numSteps;
}

Pattern duplicatePattern(const Pattern& p, const std::string& newName) {
    Pattern d = p;
    d.id = files::newId();
    d.name = newName;
    for (auto& r : d.rows) r.id = files::newId();
    return d;
}

Pattern makeVariation(const Pattern& p, uint64_t seed, float amount) {
    Pattern v = duplicatePattern(p, p.name + " Var");
    Rng rng(seed);
    amount = std::clamp(amount, 0.0f, 1.0f);
    for (auto& row : v.rows) {
        for (size_t i = 0; i < row.steps.size(); ++i) {
            auto& s = row.steps[i];
            if (s.on) {
                s.velocity = std::clamp(s.velocity + static_cast<float>(rng.uniform(-0.15, 0.15)) * amount, 0.1f, 1.0f);
                if ((row.voice == "closed_hat") && rng.uniform() < 0.25 * amount) s.roll = 2;
            } else {
                const bool offbeat = i % 2 == 1;
                if (row.voice == "closed_hat" && offbeat && rng.uniform() < 0.5 * amount) {
                    s.on = true;
                    s.velocity = 0.35f;
                } else if ((row.voice == "kick" || row.voice == "snare") && rng.uniform() < 0.08 * amount) {
                    s.on = true;
                    s.velocity = 0.3f; // ghost note
                }
            }
        }
    }
    return v;
}

bool toggleStep(Pattern& p, int row, int step) {
    if (row < 0 || row >= static_cast<int>(p.rows.size())) return false;
    auto& r = p.rows[static_cast<size_t>(row)];
    if (step < 0) return false;
    if (static_cast<int>(r.steps.size()) <= step) r.steps.resize(static_cast<size_t>(step + 1));
    auto& s = r.steps[static_cast<size_t>(step)];
    s.on = !s.on;
    return s.on;
}

double stepPosition(const Pattern& p, int step) {
    double pos = step * p.stepLengthBeats;
    if (step % 2 == 1) pos += std::clamp(p.swing, 0.0f, 1.0f) * p.stepLengthBeats / 3.0;
    return pos;
}

} // namespace beat

std::vector<PatternNote> expandPatternClip(const Pattern& pattern, const PatternClip& clip, uint64_t seed) {
    std::vector<PatternNote> out;
    const double patLen = pattern.lengthBeats();
    if (patLen <= 0 || clip.lengthBeats <= 0) return out;
    bool anySolo = false;
    for (auto& r : pattern.rows) anySolo |= r.solo;
    const double stepLen = pattern.stepLengthBeats;
    for (int rep = 0; rep * patLen < clip.lengthBeats && rep < 100000; ++rep) {
        for (size_t ri = 0; ri < pattern.rows.size(); ++ri) {
            const auto& row = pattern.rows[ri];
            if (row.muted || (anySolo && !row.solo)) continue;
            for (int si = 0; si < pattern.numSteps && si < static_cast<int>(row.steps.size()); ++si) {
                const Step& s = row.steps[static_cast<size_t>(si)];
                if (!s.on) continue;
                if (s.probability < 1.0f) {
                    Rng rng(seed ^ (static_cast<uint64_t>(rep) * 0x9E3779B97F4A7C15ull) ^ (ri << 32) ^ static_cast<uint64_t>(si * 7919 + 1));
                    rng.next();
                    if (rng.uniform() >= s.probability) continue;
                }
                const double base = rep * patLen + beat::stepPosition(pattern, si) + s.microTiming * stepLen;
                const float vel = std::clamp(s.velocity * row.volume, 0.0f, 1.0f);
                const float pan = std::clamp(row.pan + s.pan, -1.0f, 1.0f);
                const float pitch = row.pitch + s.pitch;
                const int hits = std::max(1, s.roll);
                const double sub = stepLen / hits;
                for (int h = 0; h < hits; ++h) {
                    const double b = base + h * sub;
                    if (b < 0 || b >= clip.lengthBeats) continue;
                    PatternNote n;
                    n.beat = clip.startBeat + b;
                    n.lengthBeats = std::max(0.01, sub * 0.9);
                    n.note = row.note;
                    n.velocity = h == 0 ? vel : vel * (0.75f + 0.25f * static_cast<float>(h) / hits);
                    n.pan = pan;
                    n.pitch = pitch;
                    out.push_back(n);
                }
                if (s.flam) {
                    const double b = base - stepLen * 0.12;
                    if (b >= 0 && b < clip.lengthBeats) {
                        PatternNote g;
                        g.beat = clip.startBeat + b;
                        g.lengthBeats = stepLen * 0.1;
                        g.note = row.note;
                        g.velocity = vel * 0.55f;
                        g.pan = pan;
                        g.pitch = pitch;
                        out.push_back(g);
                    }
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    return out;
}

} // namespace roy
