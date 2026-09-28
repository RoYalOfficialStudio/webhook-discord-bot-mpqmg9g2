#pragma once
// Musical scales, keys and the Wrong Note Blocker.
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace roy {

enum class ScaleType {
    Chromatic, Major, NaturalMinor, HarmonicMinor, MelodicMinor, Dorian, Phrygian, Lydian, Mixolydian, Locrian,
    MajorPentatonic, MinorPentatonic, Blues
};

struct Key {
    int root = 0; // 0 = C ... 11 = B
    ScaleType scale = ScaleType::Major;

    bool contains(int midiNote) const;
    // Nearest in-scale note (ties resolve downwards unless preferUp).
    int nearest(int midiNote, bool preferUp = false) const;
    // Nearest in-scale pitch for a fractional MIDI pitch (e.g. 60.37).
    double nearestPitch(double midiPitch) const;
    std::vector<int> pitchClasses() const;
    std::string name() const; // "C Major", "F# Minor"
};

const char* scaleTypeId(ScaleType t);              // "major", "minor", ...
const char* scaleTypeName(ScaleType t);            // "Major", "Minor", ...
std::optional<ScaleType> scaleTypeFromId(const std::string& id);
std::vector<ScaleType> allScaleTypes();
ScaleType scaleTypeByIndex(int index); // allocation-free (audio thread safe)
int scaleTypeCount();
std::array<bool, 12> scaleMask(ScaleType t);
const char* pitchClassName(int pc);                // "C", "C#", ...
std::optional<int> pitchClassFromName(const std::string& n);
std::string noteName(int midiNote);                // "C4" (C4 = 60)
// Parses "C Major", "A minor", "F# Minor", "D Dorian", "Bb major".
std::optional<Key> parseKey(const std::string& text);

// ---- Wrong Note Blocker ------------------------------------------------------
enum class WrongNoteMode { Off, Highlight, Snap, Block };
const char* wrongNoteModeId(WrongNoteMode m);
std::optional<WrongNoteMode> wrongNoteModeFromId(const std::string& id);

struct WrongNoteResult {
    bool allowed = true;     // false = note must not be created (BLOCK)
    int note = 60;           // possibly snapped note
    bool outOfScale = false; // true if the requested note is outside the key (for HIGHLIGHT)
};
// Decides what happens to a new note the user tries to place.
WrongNoteResult applyWrongNoteBlocker(const Key& key, WrongNoteMode mode, int requestedNote);

} // namespace roy
