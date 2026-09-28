#include "beat/StepSequencer.h"
#include "core/Files.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <format>

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
    pos += grooveOffsetSteps(p, step) * p.stepLengthBeats;
    return pos;
}

const std::vector<GrooveTemplate>& grooveTemplates() {
    // swing s% (MPC style): the off-beat 16th sits at s% of the 8th note -> (2s - 1) steps late.
    auto mpc = [](const char* name, float pct) {
        GrooveTemplate g{name, std::format("MPC-style 16th swing: off-beats at {:.0f}% of the 8th note", pct * 100.0f), {}, {}};
        for (int i = 0; i < 16; ++i) {
            g.timing[i] = i % 2 ? 2.0f * pct - 1.0f : 0.0f;
            g.velocity[i] = i % 2 ? 0.9f : 1.0f;
        }
        return g;
    };
    static const std::vector<GrooveTemplate> t = [&] {
        std::vector<GrooveTemplate> v;
        GrooveTemplate straight{"Straight", "no groove", {}, {}};
        for (int i = 0; i < 16; ++i) straight.velocity[i] = 1.0f;
        v.push_back(straight);
        v.push_back(mpc("MPC 54%", 0.54f));
        v.push_back(mpc("MPC 58%", 0.58f));
        v.push_back(mpc("MPC 62%", 0.62f));
        v.push_back(mpc("MPC 66%", 0.66f));
        GrooveTemplate trip{"Triplet Shuffle", "off-beats on the triplet grid (66.7% swing)", {}, {}};
        for (int i = 0; i < 16; ++i) {
            trip.timing[i] = i % 2 ? 1.0f / 3.0f : 0.0f;
            trip.velocity[i] = i % 2 ? 0.85f : 1.0f;
        }
        v.push_back(trip);
        GrooveTemplate boom{"Boom Bap Lazy", "late backbeat, dragged off-beats, quarter-note accents", {}, {}};
        for (int i = 0; i < 16; ++i) {
            boom.timing[i] = i % 2 ? 0.2f : (i % 8 == 4 ? 0.1f : 0.0f);
            boom.velocity[i] = i % 4 == 0 ? 1.0f : (i % 2 == 0 ? 0.85f : 0.7f);
        }
        v.push_back(boom);
        GrooveTemplate trap{"Trap Bounce", "straight grid, bouncing hi-hat accents", {}, {}};
        static const float trapVel[16] = {1.0f, 0.62f, 0.8f, 0.62f, 0.95f, 0.6f, 0.78f, 0.66f,
                                          1.0f, 0.62f, 0.8f, 0.62f, 0.95f, 0.6f, 0.82f, 0.7f};
        for (int i = 0; i < 16; ++i) trap.velocity[i] = trapVel[i];
        v.push_back(trap);
        GrooveTemplate drill{"Drill Push", "pushed 3rd/11th steps, slight swing, UK drill bounce", {}, {}};
        for (int i = 0; i < 16; ++i) {
            drill.timing[i] = (i == 3 || i == 11) ? -0.12f : (i % 2 ? 0.08f : 0.0f);
            drill.velocity[i] = (i == 3 || i == 11) ? 1.0f : (i % 4 == 0 ? 0.95f : 0.75f);
        }
        v.push_back(drill);
        GrooveTemplate human{"Humanize", "small fixed timing and velocity deviations", {}, {}};
        static const float ht[16] = {0.0f, 0.03f, -0.02f, 0.04f, 0.01f, -0.03f, 0.02f, 0.05f,
                                     -0.01f, 0.02f, 0.03f, -0.02f, 0.0f, 0.04f, -0.03f, 0.02f};
        static const float hv[16] = {1.0f, 0.93f, 0.97f, 0.9f, 0.98f, 0.92f, 0.96f, 0.91f,
                                     0.99f, 0.94f, 0.95f, 0.92f, 0.97f, 0.9f, 0.96f, 0.93f};
        for (int i = 0; i < 16; ++i) {
            human.timing[i] = ht[i];
            human.velocity[i] = hv[i];
        }
        v.push_back(human);
        return v;
    }();
    return t;
}

const GrooveTemplate* findGroove(const std::string& name) {
    for (auto& g : grooveTemplates())
        if (g.name == name) return &g;
    return nullptr;
}

double grooveOffsetSteps(const Pattern& p, int step) {
    const GrooveTemplate* g = findGroove(p.groove);
    if (!g || step < 0) return 0.0;
    return g->timing[step % 16] * std::clamp(p.grooveAmount, 0.0f, 1.0f);
}

float grooveVelocity(const Pattern& p, int step) {
    const GrooveTemplate* g = findGroove(p.groove);
    if (!g || step < 0) return 1.0f;
    return 1.0f + (g->velocity[step % 16] - 1.0f) * std::clamp(p.grooveAmount, 0.0f, 1.0f);
}

const std::vector<std::string>& velocityCurves() {
    static const std::vector<std::string> c = {"Flat", "Ramp Up", "Ramp Down", "Accent Downbeats", "Accent Offbeats", "Humanize"};
    return c;
}

bool applyVelocityCurve(PatternRow& row, const std::string& curve, float lo, float hi, int from, int to, uint64_t seed) {
    if (std::find(velocityCurves().begin(), velocityCurves().end(), curve) == velocityCurves().end()) return false;
    lo = std::clamp(lo, 0.0f, 1.0f);
    hi = std::clamp(hi, 0.0f, 1.0f);
    if (lo > hi) std::swap(lo, hi);
    const int n = static_cast<int>(row.steps.size());
    from = std::clamp(from, 0, std::max(0, n - 1));
    to = std::clamp(to < 0 ? n - 1 : to, from, std::max(0, n - 1));
    Rng rng(seed);
    const double span = std::max(1, to - from);
    for (int i = from; i <= to && i < n; ++i) {
        Step& s = row.steps[static_cast<size_t>(i)];
        if (!s.on) continue;
        const double x = (i - from) / span;
        double v = hi;
        if (curve == "Ramp Up") v = lo + (hi - lo) * x;
        else if (curve == "Ramp Down") v = hi - (hi - lo) * x;
        else if (curve == "Accent Downbeats") v = i % 4 == 0 ? hi : lo;
        else if (curve == "Accent Offbeats") v = i % 4 == 2 ? hi : lo;
        else if (curve == "Humanize") v = std::clamp(static_cast<double>(s.velocity) + rng.uniform(-0.08, 0.08), static_cast<double>(lo), static_cast<double>(hi));
        s.velocity = static_cast<float>(v);
    }
    return true;
}

const std::vector<std::string>& noteRepeatRates() {
    static const std::vector<std::string> r = {"1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "1/32T"};
    return r;
}

bool noteRepeat(Pattern& p, PatternRow& row, int from, int to, const std::string& rate, float velocity, std::string* error) {
    struct R { const char* name; int everySteps; int roll; int rollLength; };
    static const R table[] = {{"1/4", 4, 0, 1},  {"1/8", 2, 0, 1},  {"1/16", 1, 0, 1}, {"1/32", 1, 2, 1},
                              {"1/64", 1, 4, 1}, {"1/8T", 4, 3, 4}, {"1/16T", 2, 3, 2}, {"1/32T", 1, 3, 1}};
    const R* r = nullptr;
    for (auto& e : table)
        if (rate == e.name) r = &e;
    if (!r) {
        if (error) *error = "unknown note repeat rate " + rate;
        return false;
    }
    if (std::fabs(p.stepLengthBeats - 0.25) > 1e-9) {
        if (error) *error = "note repeat needs a 1/16 step grid";
        return false;
    }
    const int n = static_cast<int>(row.steps.size());
    if (n == 0) return true;
    from = std::clamp(from, 0, n - 1);
    to = std::clamp(to < 0 ? n - 1 : to, from, n - 1);
    velocity = std::clamp(velocity, 0.05f, 1.0f);
    for (int i = from; i <= to; ++i) {
        Step& s = row.steps[static_cast<size_t>(i)];
        const bool hit = (i - from) % r->everySteps == 0 && i + r->rollLength - 1 <= to;
        s.on = hit;
        s.roll = hit ? r->roll : 0;
        s.rollLength = hit ? r->rollLength : 1;
        if (hit) s.velocity = velocity;
    }
    return true;
}

const std::vector<std::string>& generatorStyles() {
    static const std::vector<std::string> s = {"Trap", "Boom Bap", "Drill", "House", "Afro"};
    return s;
}

bool generatePattern(Pattern& p, const std::string& style, uint64_t seed, std::string* error) {
    struct Style { const char* name; const char* groove; float swing; std::vector<std::pair<const char*, const char*>> rows; };
    static const std::vector<Style> styles = {
        {"Trap", "Trap Bounce", 0.0f, {{"kick", "x......x..x....."}, {"snare", "........x......."}, {"clap", "........x......."},
                                       {"closed_hat", "xxxxxxxxxxxxxxxx"}, {"808", "x......x..x....."}}},
        {"Boom Bap", "Boom Bap Lazy", 0.0f, {{"kick", "x.....x...x....."}, {"snare", "....x.......x..."},
                                             {"closed_hat", "x.x.x.x.x.x.x.x."}, {"open_hat", "..............x."}}},
        {"Drill", "Drill Push", 0.0f, {{"kick", "x.........x....."}, {"snare", "...x.......x...."}, {"closed_hat", "x..x..x.x..x..x."},
                                       {"808", "x.....x...x....x"}, {"perc", "......x.......x."}}},
        {"House", "MPC 54%", 0.0f, {{"kick", "x...x...x...x..."}, {"clap", "....x.......x..."}, {"closed_hat", "..x...x...x...x."},
                                    {"open_hat", "..x...x...x...x."}}},
        {"Afro", "MPC 58%", 0.0f, {{"kick", "x..x..x...x..x.."}, {"rim", "..x..x....x..x.."}, {"closed_hat", "x.xxx.xxx.xxx.xx"},
                                   {"perc", "...x......x....x"}}},
    };
    const Style* st = nullptr;
    for (auto& s : styles)
        if (style == s.name) st = &s;
    if (!st) {
        if (error) *error = "unknown style " + style;
        return false;
    }
    Rng rng(seed ? seed : 1);
    p.groove = st->groove;
    p.grooveAmount = 1.0f;
    p.swing = st->swing;
    for (auto& row : p.rows)
        for (auto& s : row.steps) s = Step{};
    for (auto& [voice, text] : st->rows) {
        PatternRow* row = nullptr;
        for (auto& r : p.rows)
            if (r.voice == voice) row = &r;
        if (!row) continue;
        const std::string t = text;
        for (size_t i = 0; i < row->steps.size(); ++i) {
            Step& s = row->steps[i];
            s.on = t[i % t.size()] == 'x';
            s.velocity = 0.85f;
        }
    }
    // seeded variation: trap/drill hat rolls, ghost notes, velocity accents - never the backbeat
    for (auto& row : p.rows) {
        for (size_t i = 0; i < row.steps.size(); ++i) {
            Step& s = row.steps[i];
            if (!s.on) {
                if ((row.voice == "kick" || row.voice == "808") && i % 4 == 3 && rng.uniform() < 0.12) {
                    s.on = true;
                    s.velocity = 0.6f;
                }
                continue;
            }
            if (row.voice == "closed_hat" && (style == "Trap" || style == "Drill") && i % 8 == 6 && rng.uniform() < 0.5) {
                s.roll = rng.uniform() < 0.5 ? 2 : 3;
                s.rollLength = s.roll == 3 ? 2 : 1;
                if (i + 1 < row.steps.size() && s.rollLength == 2) row.steps[i + 1].on = false;
            }
            if (row.voice == "closed_hat") s.velocity = static_cast<float>(rng.uniform(0.55, 0.9));
        }
    }
    return true;
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
                const float vel = std::clamp(s.velocity * row.volume * beat::grooveVelocity(pattern, si), 0.0f, 1.0f);
                const float pan = std::clamp(row.pan + s.pan, -1.0f, 1.0f);
                const float pitch = row.pitch + s.pitch;
                const int hits = std::max(1, s.roll);
                const double sub = stepLen * std::max(1, s.rollLength) / hits;
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
