#pragma once
// IMPORT BEAT: a finished beat (bought / downloaded MP3, WAV, FLAC ...) goes onto its own audio
// track from bar 1. Before importing, RoY reads tempo and key from the file name ("Night 140 BPM
// Am.mp3") and, as a fallback, estimates them from the audio, so the song tempo (grid, metronome)
// and the song key (LIVE VOCAL autotune) can match the beat.
#include "midi/Scale.h"

#include <filesystem>
#include <optional>
#include <string>

namespace roy::beatimport {

namespace fs = std::filesystem;

// "Trap Beat 140 BPM" / "140bpm" / "140_BPM" -> 140. Only numbers next to "bpm" count.
std::optional<double> bpmFromFileName(const std::string& stem);
// "Am", "A min", "F#m", "Bb Minor", "C maj", "Eb Major", "(Key C#m)" -> key. A bare letter is
// ignored (too ambiguous: "A Beat").
std::optional<Key> keyFromFileName(const std::string& stem);

struct BeatFileInfo {
    bool ok = false;
    std::string error;
    double seconds = 0, sampleRate = 0;
    int channels = 0;
    std::optional<double> bpmFromName;
    std::optional<Key> keyFromName;
    double bpmDetected = 0, bpmConfidence = 0; // 60..180 (a 140 BPM trap beat may show as 70)
    int keyRoot = -1;
    bool keyMinor = false;
    double keyConfidence = 0;
    // What RoY suggests (name first, then a confident detection); 0 / nullopt = nothing reliable.
    double suggestedBpm() const;
    std::optional<Key> suggestedKey() const;
};
// Decodes the file and analyses it (can take ~1 s for a 3-minute MP3: run it off the UI thread).
BeatFileInfo analyzeBeatFile(const fs::path& file);
bool isImportableAudio(const fs::path& file); // .mp3 .wav .flac .ogg .aif(f)

} // namespace roy::beatimport
