#pragma once
// Takes, take lanes and comping on the project model.
#include "project/Project.h"
#include "record/Recorder.h"

#include <filesystem>
#include <string>
#include <vector>

namespace roy::takes {

// Armed tracks -> recorder configuration.
std::vector<RecordTrackConfig> recordConfig(const Project& p);

// Registers a finished take: new asset (kind "recording"), a Take on the next
// free lane, and makes it the active comp over its range (latest take wins).
// Returns the take id.
std::string addRecordedTake(Project& p, const FinishedTake& t, const std::filesystem::path& projectFolder);

// Selects `takeId` for [startBeat, endBeat) in the comp, splitting others.
bool compSelect(Track& t, const std::string& takeId, double startBeat, double endBeat);
// Whole take becomes the comp over its own range.
bool compWholeTake(Track& t, const std::string& takeId);
// Converts the comp into regular audio clips (with short crossfades) and clears it.
std::vector<std::string> flattenComp(Project& p, const std::string& trackId);
// Deletes a take from the model (the audio FILE is kept on disk).
bool removeTake(Track& t, const std::string& takeId);

// Adds recovered never-lose audio as an asset + clip on the track.
// Places it at its original timeline position when known, else at `fallbackBeat`.
std::string addRecoveredPerformance(Project& p, const RecoveredPerformance& r, const std::filesystem::path& projectFolder,
                                    double fallbackBeat);

} // namespace roy::takes
