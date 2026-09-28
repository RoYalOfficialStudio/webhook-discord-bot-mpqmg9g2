#pragma once
// MP3 export through LAME (libmp3lame 3.100, LGPL-2.0).
// LAME is NOT linked into RoY Studio: it is a separate shared library
// (roy_mp3lame.dll / roy_mp3lame.so, built from the vendored, unmodified sources)
// loaded at runtime. If it is missing, MP3 export reports a clear error and
// everything else keeps working. Users may replace the library (LGPL).
#include <filesystem>
#include <string>
#include <vector>

namespace roy::mp3 {

struct Options {
    int bitrateKbps = 320;  // CBR: 32..320 (MPEG-1 layer III rates); VBR: ignored
    bool vbr = false;
    int vbrQuality = 2;     // 0 = best .. 9 = smallest
    int quality = 2;        // encoder algorithm quality 0 (best, slow) .. 9
    // metadata (ID3v2.3 + ID3v1)
    std::string title, artist, album, year, comment, track, genre;
};

// Loads the library if needed. `why` explains a failure (path searched, missing symbol).
bool available(std::string* why = nullptr);
std::string encoderVersion();
// Valid CBR bitrates for MPEG-1 layer III.
bool validBitrate(int kbps);
// Sample rates LAME accepts without resampling for MPEG-1 (32/44.1/48 kHz) and 2/2.5 (lower).
bool supportedSampleRate(int hz);

// channels: 1 (mono) or 2 (stereo), float -1..1. Never overwrites an existing file.
bool encode(const std::filesystem::path& out, const std::vector<std::vector<float>>& channels, int sampleRate, const Options& o,
            std::string* error = nullptr);

} // namespace roy::mp3
