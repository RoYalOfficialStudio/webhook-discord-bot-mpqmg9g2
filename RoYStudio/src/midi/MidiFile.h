#pragma once
// Standard MIDI File (SMF) import/export, format 0 and 1.
#include "audio/TempoMap.h"
#include "project/Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace roy::midi {

struct SmfTrack {
    std::string name;
    std::vector<MidiNote> notes; // beats from the file start
    int channel = 0;
};

struct SmfData {
    int format = 1;
    int ppq = 960;
    TempoMap tempo;
    std::vector<SmfTrack> tracks;
};

bool readMidiFile(const std::filesystem::path& path, SmfData& out, std::string* error = nullptr);
bool parseMidi(const std::vector<uint8_t>& bytes, SmfData& out, std::string* error = nullptr);
std::vector<uint8_t> buildMidi(const SmfData& data);
bool writeMidiFile(const std::filesystem::path& path, const SmfData& data, bool allowOverwrite = false,
                   std::string* error = nullptr);

// Convenience: export one clip (with the project tempo map) / all MIDI tracks.
SmfData clipToSmf(const Project& p, const MidiClip& clip, const std::string& name);
SmfData projectToSmf(const Project& p);
// Import: creates MIDI tracks with one clip each. Returns created track ids.
std::vector<std::string> importSmfIntoProject(Project& p, const SmfData& d, double atBeat = 0.0, bool applyTempo = false);

} // namespace roy::midi
