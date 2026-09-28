#include "midi/Scale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace roy {

namespace {
struct ScaleDef {
    ScaleType type;
    const char* id;
    const char* name;
    std::vector<int> intervals;
};

const std::vector<ScaleDef>& defs() {
    static const std::vector<ScaleDef> d = {
        {ScaleType::Chromatic, "chromatic", "Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {ScaleType::Major, "major", "Major", {0, 2, 4, 5, 7, 9, 11}},
        {ScaleType::NaturalMinor, "minor", "Minor", {0, 2, 3, 5, 7, 8, 10}},
        {ScaleType::HarmonicMinor, "harmonic_minor", "Harmonic Minor", {0, 2, 3, 5, 7, 8, 11}},
        {ScaleType::MelodicMinor, "melodic_minor", "Melodic Minor", {0, 2, 3, 5, 7, 9, 11}},
        {ScaleType::Dorian, "dorian", "Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {ScaleType::Phrygian, "phrygian", "Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {ScaleType::Lydian, "lydian", "Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {ScaleType::Mixolydian, "mixolydian", "Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {ScaleType::Locrian, "locrian", "Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {ScaleType::MajorPentatonic, "major_pentatonic", "Major Pentatonic", {0, 2, 4, 7, 9}},
        {ScaleType::MinorPentatonic, "minor_pentatonic", "Minor Pentatonic", {0, 3, 5, 7, 10}},
        {ScaleType::Blues, "blues", "Blues", {0, 3, 5, 6, 7, 10}},
    };
    return d;
}

const ScaleDef& def(ScaleType t) {
    for (auto& d : defs())
        if (d.type == t) return d;
    return defs()[1];
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::array<bool, 12> scaleMask(ScaleType t) {
    std::array<bool, 12> m{};
    for (int i : def(t).intervals) m[static_cast<size_t>(i)] = true;
    return m;
}

bool Key::contains(int midiNote) const {
    const int pc = ((midiNote - root) % 12 + 12) % 12;
    return scaleMask(scale)[static_cast<size_t>(pc)];
}

int Key::nearest(int midiNote, bool preferUp) const {
    if (contains(midiNote)) return midiNote;
    for (int d = 1; d <= 6; ++d) {
        const int a = preferUp ? midiNote + d : midiNote - d;
        const int b = preferUp ? midiNote - d : midiNote + d;
        if (contains(a)) return a;
        if (contains(b)) return b;
    }
    return midiNote;
}

double Key::nearestPitch(double midiPitch) const {
    const int lo = static_cast<int>(std::floor(midiPitch));
    double best = midiPitch;
    double bestDist = 1e9;
    for (int n = lo - 6; n <= lo + 7; ++n) {
        if (!contains(n)) continue;
        const double d = std::fabs(n - midiPitch);
        if (d < bestDist) {
            bestDist = d;
            best = n;
        }
    }
    return best;
}

std::vector<int> Key::pitchClasses() const {
    std::vector<int> out;
    for (int i : def(scale).intervals) out.push_back((root + i) % 12);
    return out;
}

std::string Key::name() const { return std::format("{} {}", pitchClassName(root), scaleTypeName(scale)); }

const char* scaleTypeId(ScaleType t) { return def(t).id; }
const char* scaleTypeName(ScaleType t) { return def(t).name; }

std::optional<ScaleType> scaleTypeFromId(const std::string& id) {
    const std::string l = lower(id);
    for (auto& d : defs())
        if (l == d.id || l == lower(d.name)) return d.type;
    if (l == "natural_minor" || l == "aeolian" || l == "natural minor") return ScaleType::NaturalMinor;
    if (l == "ionian") return ScaleType::Major;
    return std::nullopt;
}

std::vector<ScaleType> allScaleTypes() {
    std::vector<ScaleType> v;
    for (auto& d : defs()) v.push_back(d.type);
    return v;
}

const char* pitchClassName(int pc) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((pc % 12) + 12) % 12];
}

std::optional<int> pitchClassFromName(const std::string& n) {
    if (n.empty()) return std::nullopt;
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    static const int base[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    if (c < 'A' || c > 'G') return std::nullopt;
    int pc = base[c - 'A'];
    for (size_t i = 1; i < n.size(); ++i) {
        if (n[i] == '#') ++pc;
        else if (n[i] == 'b') --pc;
        else return std::nullopt;
    }
    return ((pc % 12) + 12) % 12;
}

std::string noteName(int midiNote) {
    const int octave = (midiNote >= 0 ? midiNote / 12 : (midiNote - 11) / 12) - 1;
    return std::format("{}{}", pitchClassName(midiNote), octave);
}

std::optional<Key> parseKey(const std::string& text) {
    const auto sp = text.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    auto pc = pitchClassFromName(text.substr(0, sp));
    auto sc = scaleTypeFromId(text.substr(sp + 1));
    if (!pc || !sc) return std::nullopt;
    return Key{*pc, *sc};
}

const char* wrongNoteModeId(WrongNoteMode m) {
    switch (m) {
    case WrongNoteMode::Off: return "off";
    case WrongNoteMode::Highlight: return "highlight";
    case WrongNoteMode::Snap: return "snap";
    case WrongNoteMode::Block: return "block";
    }
    return "off";
}

std::optional<WrongNoteMode> wrongNoteModeFromId(const std::string& id) {
    const auto l = lower(id);
    if (l == "off") return WrongNoteMode::Off;
    if (l == "highlight") return WrongNoteMode::Highlight;
    if (l == "snap") return WrongNoteMode::Snap;
    if (l == "block") return WrongNoteMode::Block;
    return std::nullopt;
}

WrongNoteResult applyWrongNoteBlocker(const Key& key, WrongNoteMode mode, int requestedNote) {
    WrongNoteResult r;
    r.note = requestedNote;
    r.outOfScale = !key.contains(requestedNote);
    if (!r.outOfScale || mode == WrongNoteMode::Off || mode == WrongNoteMode::Highlight) return r;
    if (mode == WrongNoteMode::Snap) {
        r.note = key.nearest(requestedNote);
        return r;
    }
    r.allowed = false; // BLOCK
    return r;
}

} // namespace roy
