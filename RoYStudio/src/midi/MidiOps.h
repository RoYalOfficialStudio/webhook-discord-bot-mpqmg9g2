#pragma once
// Piano-roll editing operations on MIDI notes (clip-relative beats).
// Operations take a note selection (indices); an empty selection = all notes.
#include "midi/Scale.h"
#include "project/Project.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace roy::midi {

using Selection = std::vector<size_t>;

// Adds a note, honouring the Wrong Note Blocker. Returns the index or nullopt if blocked.
std::optional<size_t> addNote(MidiClip& clip, MidiNote note, const Key& key, WrongNoteMode mode);

// strength 0..1 (1 = hard quantize), optional end quantize, swing 0..1 on off-beats.
void quantize(MidiClip& clip, const Selection& sel, double gridBeats, double strength = 1.0, bool quantizeEnds = false,
              double swing = 0.0);
// Random timing (beats, +-) and velocity (+- units) variation, deterministic by seed.
void humanize(MidiClip& clip, const Selection& sel, double timingBeats, int velocityRange, uint64_t seed);
// Chromatic transpose; if `diatonicKey` is given, moves by scale steps instead.
void transpose(MidiClip& clip, const Selection& sel, int amount, const Key* diatonicKey = nullptr);
// Copies the selection `times` times after itself. Returns new indices.
Selection duplicate(MidiClip& clip, const Selection& sel, int times = 1);
// Extends each note to the start of the next (monophonic legato) - chords kept together.
void legato(MidiClip& clip, const Selection& sel);
// Offsets notes that start together (chords) by `stepBeats` per voice. up=true: low to high.
void strum(MidiClip& clip, const Selection& sel, double stepBeats, bool up = true);
enum class ArpMode { Up, Down, UpDown, Random, AsPlayed };
// Replaces each chord by an arpeggio of `rateBeats` notes with gate 0..1 over the chord length.
void arpeggiate(MidiClip& clip, const Selection& sel, double rateBeats, ArpMode mode, double gate = 0.9, uint64_t seed = 1);
// Snaps out-of-key notes to the key (used when switching Wrong Note mode to SNAP).
int snapToKey(MidiClip& clip, const Selection& sel, const Key& key);
std::vector<bool> outOfKey(const MidiClip& clip, const Key& key); // for HIGHLIGHT
void setVelocity(MidiClip& clip, const Selection& sel, int velocity);
void scaleVelocity(MidiClip& clip, const Selection& sel, double factor);
void setLength(MidiClip& clip, const Selection& sel, double lengthBeats);
void removeNotes(MidiClip& clip, const Selection& sel);
void sortNotes(MidiClip& clip);

// Notes of other MIDI clips overlapping the clip's time range, converted to its time base.
std::vector<MidiNote> ghostNotes(const Project& p, const std::string& clipId);

// ---- chord detection -----------------------------------------------------------
struct Chord {
    int root = -1;          // pitch class, -1 = none
    std::string quality;    // "maj", "min", "7", ...
    std::string name;       // "C#m7"
    int bass = -1;          // pitch class of the lowest note (slash chords)
};
Chord detectChord(const std::vector<int>& midiNotes);
// Chords over time: one entry per change (clip-relative beats).
struct ChordAt {
    double beat = 0;
    Chord chord;
};
std::vector<ChordAt> detectChords(const MidiClip& clip, double resolutionBeats = 0.5);

} // namespace roy::midi
