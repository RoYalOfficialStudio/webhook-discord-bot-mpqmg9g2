#pragma once
// EXPORT: mixdown and stems to WAV / FLAC with sample-rate conversion,
// bit depth, dither, normalisation, tail, range selection.
// MP3 is not available: it needs an MP3 encoder library whose licence
// (e.g. LAME, LGPL) must be approved first - reported as an error, never faked.
#include "dsp/Loudness.h"
#include "project/Project.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace roy {
class AudioEngine;
class ProjectRuntime;
}

namespace roy::exporting {

enum class Format { Wav, Flac, Mp3 };
enum class Dither { None, Tpdf, TpdfShaped };
enum class Normalize { None, Peak, Loudness };
enum class Range { FullSong, Selection, Loop };
enum class Stems { None, AllTracks, SelectedTracks, MixerBusses, VocalStems, Instrumental };

struct ExportOptions {
    Format format = Format::Wav;
    double sampleRate = 0;        // 0 = project/engine rate
    int bitDepth = 24;            // 16 | 24 | 32 (32 = float for WAV)
    Dither dither = Dither::Tpdf; // applied for 16/24-bit integer output
    Normalize normalize = Normalize::None;
    double normalizePeakDb = -1.0;
    double normalizeLufs = -14.0;
    double truePeakCeilingDb = -1.0;
    double tailSeconds = -1.0;    // <0 = automatic (effect tails)
    Range range = Range::FullSong;
    double startBeat = 0, endBeat = 0; // for Selection
    bool mixdown = true;
    Stems stems = Stems::None;
    std::vector<std::string> selectedTrackIds;
    std::filesystem::path folder;
    std::string baseName;
    std::function<bool(double)> progress;
};

struct ExportedFile {
    std::filesystem::path path;
    std::string what; // "mixdown", track name, bus name, "instrumental", ...
    dsp::LoudnessStats stats;
    double appliedGainDb = 0;
};

struct ExportResult {
    bool ok = false;
    std::string error;
    std::vector<ExportedFile> files;
    std::vector<std::string> warnings;
    double renderedSeconds = 0;
};

ExportResult exportProject(AudioEngine& engine, ProjectRuntime& runtime, Project& project, const ExportOptions& options);

// Building blocks (also used by tests / other features)
std::vector<std::vector<int32_t>> quantize(const std::vector<std::vector<float>>& in, int bits, Dither dither, uint64_t seed = 1);
bool writeAudio(const std::filesystem::path& path, const std::vector<std::vector<float>>& audio, double sampleRate, Format fmt,
                int bitDepth, Dither dither, std::string* error = nullptr);
double automaticTailSeconds(ProjectRuntime& runtime, const Project& project);
const char* extensionFor(Format f);

} // namespace roy::exporting
