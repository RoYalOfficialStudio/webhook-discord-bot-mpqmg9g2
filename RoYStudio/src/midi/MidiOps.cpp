#include "midi/MidiOps.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace roy::midi {

namespace {
Selection resolve(const MidiClip& clip, const Selection& sel) {
    if (!sel.empty()) {
        Selection s;
        for (auto i : sel)
            if (i < clip.notes.size()) s.push_back(i);
        return s;
    }
    Selection all(clip.notes.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    return all;
}

// Groups selected notes that start at (almost) the same time -> chords.
std::vector<std::vector<size_t>> chordGroups(const MidiClip& clip, const Selection& sel, double tol = 1e-3) {
    Selection s = resolve(clip, sel);
    std::sort(s.begin(), s.end(), [&](size_t a, size_t b) { return clip.notes[a].startBeat < clip.notes[b].startBeat; });
    std::vector<std::vector<size_t>> groups;
    for (size_t i : s) {
        if (!groups.empty() && std::fabs(clip.notes[groups.back().front()].startBeat - clip.notes[i].startBeat) <= tol)
            groups.back().push_back(i);
        else
            groups.push_back({i});
    }
    return groups;
}
} // namespace

std::optional<size_t> addNote(MidiClip& clip, MidiNote note, const Key& key, WrongNoteMode mode) {
    auto r = applyWrongNoteBlocker(key, mode, note.pitch);
    if (!r.allowed) return std::nullopt;
    note.pitch = std::clamp(r.note, 0, 127);
    note.velocity = std::clamp(note.velocity, 1, 127);
    note.lengthBeats = std::max(1e-3, note.lengthBeats);
    note.startBeat = std::max(0.0, note.startBeat);
    clip.notes.push_back(note);
    return clip.notes.size() - 1;
}

void quantize(MidiClip& clip, const Selection& sel, double grid, double strength, bool ends, double swing) {
    if (grid <= 0) return;
    strength = std::clamp(strength, 0.0, 1.0);
    for (size_t i : resolve(clip, sel)) {
        auto& n = clip.notes[i];
        const double idx = std::round(n.startBeat / grid);
        double target = idx * grid;
        if (swing > 0 && static_cast<int64_t>(idx) % 2 != 0) target += std::clamp(swing, 0.0, 1.0) * grid / 3.0;
        const double end = n.endBeat();
        n.startBeat += (target - n.startBeat) * strength;
        if (ends) {
            const double qEnd = std::max(target + grid, std::round(end / grid) * grid);
            const double newEnd = end + (qEnd - end) * strength;
            n.lengthBeats = std::max(grid * 0.25, newEnd - n.startBeat);
        } else {
            n.lengthBeats = std::max(1e-3, end - n.startBeat);
        }
        n.startBeat = std::max(0.0, n.startBeat);
    }
}

void humanize(MidiClip& clip, const Selection& sel, double timingBeats, int velocityRange, uint64_t seed) {
    Rng rng(seed);
    for (size_t i : resolve(clip, sel)) {
        auto& n = clip.notes[i];
        n.startBeat = std::max(0.0, n.startBeat + rng.uniform(-timingBeats, timingBeats));
        n.velocity = std::clamp(n.velocity + static_cast<int>(std::lround(rng.uniform(-velocityRange, velocityRange))), 1, 127);
    }
}

void transpose(MidiClip& clip, const Selection& sel, int amount, const Key* key) {
    for (size_t i : resolve(clip, sel)) {
        auto& n = clip.notes[i];
        if (!key) {
            n.pitch = std::clamp(n.pitch + amount, 0, 127);
            continue;
        }
        int p = key->nearest(n.pitch);
        const int dir = amount >= 0 ? 1 : -1;
        for (int step = 0; step < std::abs(amount); ++step) {
            do { p += dir; } while (!key->contains(p) && p > 0 && p < 127);
        }
        n.pitch = std::clamp(p, 0, 127);
    }
}

Selection duplicate(MidiClip& clip, const Selection& sel, int times) {
    Selection s = resolve(clip, sel);
    Selection created;
    if (s.empty()) return created;
    double lo = 1e18, hi = -1e18;
    for (size_t i : s) {
        lo = std::min(lo, clip.notes[i].startBeat);
        hi = std::max(hi, clip.notes[i].endBeat());
    }
    // Duplicate by whole bars of the selection length rounded up to a beat.
    const double span = std::max(1.0, std::ceil((hi - lo) - 1e-9));
    const std::vector<MidiNote> src = [&] {
        std::vector<MidiNote> v;
        for (size_t i : s) v.push_back(clip.notes[i]);
        return v;
    }();
    for (int t = 1; t <= times; ++t)
        for (auto n : src) {
            n.startBeat += span * t;
            clip.notes.push_back(n);
            created.push_back(clip.notes.size() - 1);
        }
    const double newEnd = lo + span * (times + 1);
    if (clip.lengthBeats < newEnd) clip.lengthBeats = newEnd;
    return created;
}

void legato(MidiClip& clip, const Selection& sel) {
    auto groups = chordGroups(clip, sel);
    for (size_t g = 0; g + 1 < groups.size(); ++g) {
        const double nextStart = clip.notes[groups[g + 1].front()].startBeat;
        for (size_t i : groups[g]) clip.notes[i].lengthBeats = std::max(1e-3, nextStart - clip.notes[i].startBeat);
    }
}

void strum(MidiClip& clip, const Selection& sel, double stepBeats, bool up) {
    for (auto& g : chordGroups(clip, sel)) {
        if (g.size() < 2) continue;
        std::sort(g.begin(), g.end(), [&](size_t a, size_t b) {
            return up ? clip.notes[a].pitch < clip.notes[b].pitch : clip.notes[a].pitch > clip.notes[b].pitch;
        });
        for (size_t k = 0; k < g.size(); ++k) {
            auto& n = clip.notes[g[k]];
            const double end = n.endBeat();
            n.startBeat += stepBeats * static_cast<double>(k);
            n.lengthBeats = std::max(1e-3, end - n.startBeat);
        }
    }
}

void arpeggiate(MidiClip& clip, const Selection& sel, double rate, ArpMode mode, double gate, uint64_t seed) {
    if (rate <= 0) return;
    auto groups = chordGroups(clip, sel);
    Rng rng(seed);
    std::vector<MidiNote> result;
    std::set<size_t> consumed;
    for (auto& g : groups) {
        std::vector<MidiNote> chord;
        double len = 0;
        for (size_t i : g) {
            chord.push_back(clip.notes[i]);
            len = std::max(len, clip.notes[i].lengthBeats);
            consumed.insert(i);
        }
        std::vector<MidiNote> order = chord;
        if (mode != ArpMode::AsPlayed)
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.pitch < b.pitch; });
        if (mode == ArpMode::Down) std::reverse(order.begin(), order.end());
        if (mode == ArpMode::UpDown && order.size() > 2)
            for (size_t k = order.size() - 2; k >= 1; --k) order.push_back(order[k]);
        const double start = chord.front().startBeat;
        const int steps = std::max(1, static_cast<int>(std::floor(len / rate + 1e-9)));
        for (int s = 0; s < steps; ++s) {
            MidiNote n = mode == ArpMode::Random ? order[static_cast<size_t>(rng.next() % order.size())] : order[static_cast<size_t>(s) % order.size()];
            n.startBeat = start + s * rate;
            n.lengthBeats = rate * std::clamp(gate, 0.05, 1.0);
            result.push_back(n);
        }
    }
    std::vector<MidiNote> kept;
    for (size_t i = 0; i < clip.notes.size(); ++i)
        if (!consumed.count(i)) kept.push_back(clip.notes[i]);
    kept.insert(kept.end(), result.begin(), result.end());
    clip.notes = std::move(kept);
    sortNotes(clip);
}

int snapToKey(MidiClip& clip, const Selection& sel, const Key& key) {
    int changed = 0;
    for (size_t i : resolve(clip, sel)) {
        auto& n = clip.notes[i];
        const int q = key.nearest(n.pitch);
        if (q != n.pitch) {
            n.pitch = q;
            ++changed;
        }
    }
    return changed;
}

std::vector<bool> outOfKey(const MidiClip& clip, const Key& key) {
    std::vector<bool> v(clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); ++i) v[i] = !key.contains(clip.notes[i].pitch);
    return v;
}

void setVelocity(MidiClip& clip, const Selection& sel, int velocity) {
    for (size_t i : resolve(clip, sel)) clip.notes[i].velocity = std::clamp(velocity, 1, 127);
}

void scaleVelocity(MidiClip& clip, const Selection& sel, double f) {
    for (size_t i : resolve(clip, sel)) clip.notes[i].velocity = std::clamp(static_cast<int>(std::lround(clip.notes[i].velocity * f)), 1, 127);
}

void setLength(MidiClip& clip, const Selection& sel, double len) {
    for (size_t i : resolve(clip, sel)) clip.notes[i].lengthBeats = std::max(1e-3, len);
}

void removeNotes(MidiClip& clip, const Selection& sel) {
    Selection s = resolve(clip, sel);
    std::sort(s.rbegin(), s.rend());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    for (size_t i : s) clip.notes.erase(clip.notes.begin() + static_cast<long>(i));
}

void sortNotes(MidiClip& clip) {
    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](auto& a, auto& b) {
        return a.startBeat != b.startBeat ? a.startBeat < b.startBeat : a.pitch < b.pitch;
    });
}

std::vector<MidiNote> ghostNotes(const Project& p, const std::string& clipId) {
    const MidiClip* self = nullptr;
    for (auto& t : p.tracks)
        for (auto& c : t.midiClips)
            if (c.id == clipId) self = &c;
    std::vector<MidiNote> out;
    if (!self) return out;
    for (auto& t : p.tracks)
        for (auto& c : t.midiClips) {
            if (c.id == clipId || c.endBeat() <= self->startBeat || c.startBeat >= self->endBeat()) continue;
            for (auto n : c.notes) {
                n.startBeat += c.startBeat - self->startBeat;
                if (n.endBeat() <= 0 || n.startBeat >= self->lengthBeats) continue;
                out.push_back(n);
            }
        }
    return out;
}

// ---------------------------------------------------------------- chords
namespace {
struct ChordTemplate {
    const char* quality;
    const char* suffix;
    std::vector<int> intervals;
};
const std::vector<ChordTemplate>& templates() {
    static const std::vector<ChordTemplate> t = {
        {"maj", "", {0, 4, 7}},        {"min", "m", {0, 3, 7}},          {"dim", "dim", {0, 3, 6}},
        {"aug", "aug", {0, 4, 8}},     {"sus2", "sus2", {0, 2, 7}},      {"sus4", "sus4", {0, 5, 7}},
        {"7", "7", {0, 4, 7, 10}},     {"maj7", "maj7", {0, 4, 7, 11}},  {"m7", "m7", {0, 3, 7, 10}},
        {"m7b5", "m7b5", {0, 3, 6, 10}}, {"dim7", "dim7", {0, 3, 6, 9}}, {"mmaj7", "mMaj7", {0, 3, 7, 11}},
        {"6", "6", {0, 4, 7, 9}},      {"m6", "m6", {0, 3, 7, 9}},       {"add9", "add9", {0, 2, 4, 7}},
        {"madd9", "madd9", {0, 2, 3, 7}}, {"9", "9", {0, 2, 4, 7, 10}},  {"maj9", "maj9", {0, 2, 4, 7, 11}},
        {"m9", "m9", {0, 2, 3, 7, 10}}, {"5", "5", {0, 7}}};
    return t;
}
} // namespace

Chord detectChord(const std::vector<int>& notes) {
    Chord best;
    if (notes.empty()) return best;
    std::set<int> pcs;
    int lowest = 1000;
    for (int n : notes) {
        pcs.insert(((n % 12) + 12) % 12);
        lowest = std::min(lowest, n);
    }
    const int bass = ((lowest % 12) + 12) % 12;
    if (pcs.size() == 1) return best; // single note: no chord
    double bestScore = -1e9;
    for (int root = 0; root < 12; ++root) {
        for (auto& t : templates()) {
            std::set<int> tpl;
            for (int iv : t.intervals) tpl.insert((root + iv) % 12);
            int hit = 0, extra = 0, missing = 0;
            for (int pc : pcs) (tpl.count(pc) ? hit : extra)++;
            for (int pc : tpl) if (!pcs.count(pc)) ++missing;
            if (!pcs.count(root)) continue; // the root must be played
            double score = hit * 2.0 - extra * 2.5 - missing * 1.5 - t.intervals.size() * 0.01;
            if (root == bass) score += 0.6;
            if (score > bestScore) {
                bestScore = score;
                best.root = root;
                best.quality = t.quality;
                best.name = std::string(pitchClassName(root)) + t.suffix;
            }
        }
    }
    best.bass = bass;
    if (best.root >= 0 && bass != best.root) best.name += std::string("/") + pitchClassName(bass);
    return best;
}

std::vector<ChordAt> detectChords(const MidiClip& clip, double res) {
    std::vector<ChordAt> out;
    if (res <= 0) return out;
    for (double b = 0; b < clip.lengthBeats; b += res) {
        std::vector<int> sounding;
        for (auto& n : clip.notes)
            if (!n.muted && n.startBeat <= b + 1e-9 && n.endBeat() > b + 1e-9) sounding.push_back(n.pitch);
        Chord c = detectChord(sounding);
        if (out.empty() || out.back().chord.name != c.name) out.push_back({b, c});
    }
    return out;
}

std::optional<MidiClip> clipFromLiveRecording(const std::vector<TimedMidi>& evIn, const TempoMap& tempo, double sr, int64_t endTimeline,
                                              double quantizeBeats) {
    auto ev = evIn;
    std::stable_sort(ev.begin(), ev.end(), [](auto& a, auto& b) { return a.timeline < b.timeline; });
    auto beatOf = [&](int64_t t) { return tempo.sampleToBeat(static_cast<double>(t), sr); };
    struct Open {
        double start = -1;
        int velocity = 0;
        bool pedalHeld = false; // key released while the pedal was down
    };
    Open open[16][128];
    bool pedal[16] = {};
    std::vector<MidiNote> notes; // absolute beats for now
    auto finish = [&](int ch, int n, double end) {
        Open& o = open[ch][n];
        if (o.start < 0) return;
        MidiNote m;
        m.pitch = n;
        m.channel = ch;
        m.velocity = std::clamp(o.velocity, 1, 127);
        m.startBeat = o.start;
        m.lengthBeats = std::max(1.0 / 64.0, end - o.start);
        notes.push_back(m);
        o = Open{};
    };
    for (auto& e : ev) {
        const int ch = e.status & 0x0F, type = e.status & 0xF0, n = e.data1 & 0x7F;
        const double b = beatOf(e.timeline);
        if (type == 0x90 && e.data2 > 0) {
            finish(ch, n, b); // re-strike ends the previous one
            open[ch][n] = Open{b, e.data2, false};
        } else if (type == 0x80 || type == 0x90) {
            if (pedal[ch] && open[ch][n].start >= 0) open[ch][n].pedalHeld = true;
            else finish(ch, n, b);
        } else if (type == 0xB0 && e.data1 == 64) {
            const bool down = e.data2 >= 64;
            if (pedal[ch] && !down)
                for (int k = 0; k < 128; ++k)
                    if (open[ch][k].pedalHeld) finish(ch, k, b);
            pedal[ch] = down;
        }
    }
    const double endBeat = beatOf(endTimeline);
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n) finish(ch, n, std::max(endBeat, open[ch][n].start + 1.0 / 64.0));
    if (notes.empty()) return std::nullopt;
    std::sort(notes.begin(), notes.end(), [](auto& a, auto& b) { return a.startBeat < b.startBeat || (a.startBeat == b.startBeat && a.pitch < b.pitch); });
    if (quantizeBeats > 0)
        for (auto& m : notes) m.startBeat = std::max(0.0, std::round(m.startBeat / quantizeBeats) * quantizeBeats);
    double first = notes.front().startBeat, last = 0;
    for (auto& m : notes) last = std::max(last, m.endBeat());
    // clip on bar boundaries (4/4 fallback if the signature is odd)
    const auto bb = tempo.beatToBarBeat(first);
    const double barStart = tempo.barToBeat(bb.bar);
    const auto sig = tempo.signatureAtBar(bb.bar);
    const double barLen = TempoMap::barLengthBeats(sig.numerator, sig.denominator);
    MidiClip c;
    c.name = "Recorded MIDI";
    c.startBeat = std::max(0.0, barStart);
    c.lengthBeats = std::max(barLen, std::ceil((last - c.startBeat) / barLen - 1e-9) * barLen);
    for (auto& m : notes) {
        m.startBeat -= c.startBeat;
        c.notes.push_back(m);
    }
    return c;
}

} // namespace roy::midi
