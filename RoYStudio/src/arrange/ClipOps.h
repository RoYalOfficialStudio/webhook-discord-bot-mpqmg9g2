#pragma once
// Playlist / arrangement editing operations on the project model.
// All operations are non-destructive: they only change clip metadata; audio
// files are never modified. Locked clips refuse edits.
#include "project/Project.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace roy::arrange {

// ---- time / grid -----------------------------------------------------------
double snap(double beat, double gridBeats);
enum class TimeFormat { BarsBeats, Seconds, Samples };
std::string formatTime(const Project& p, double beat, TimeFormat f);

// Generic view onto an audio, MIDI or pattern clip.
struct ClipRef {
    Track* track = nullptr;
    AudioClip* audio = nullptr;
    MidiClip* midi = nullptr;
    PatternClip* pattern = nullptr;
    explicit operator bool() const { return track && (audio || midi || pattern); }
    double& start();
    double& length();
    bool locked() const;
    const std::string& groupId() const;
    const std::string& id() const;
};
ClipRef findClip(Project& p, const std::string& clipId);

struct EditResult {
    bool ok = false;
    std::string error;
    std::string newId; // created clip (split/duplicate/copy)
    static EditResult fail(std::string e) { return {false, std::move(e), {}}; }
};

// Length available in the source for an audio clip (seconds of timeline).
double maxAudioClipLengthBeats(const Project& p, const AudioClip& c);

EditResult moveClip(Project& p, const std::string& clipId, double newStartBeat, const std::string& targetTrackId = {},
                    bool moveGroup = true);
EditResult duplicateClip(Project& p, const std::string& clipId, std::optional<double> startBeat = std::nullopt);
EditResult copyClipTo(Project& p, const std::string& clipId, const std::string& trackId, double startBeat);
EditResult splitClip(Project& p, const std::string& clipId, double atBeat);
EditResult trimClipStart(Project& p, const std::string& clipId, double newStartBeat);
EditResult trimClipEnd(Project& p, const std::string& clipId, double newEndBeat);
// Moves audio content inside the clip boundaries (positive = content later).
EditResult slipClip(Project& p, const std::string& clipId, double deltaSeconds);
// Time-stretch: the clip plays `ratio` times longer, pitch preserved.
EditResult stretchClip(Project& p, const std::string& clipId, double ratio);
EditResult deleteClip(Project& p, const std::string& clipId);
EditResult setClipMuted(Project& p, const std::string& clipId, bool muted);
EditResult setClipLocked(Project& p, const std::string& clipId, bool locked);
EditResult setClipColor(Project& p, const std::string& clipId, uint32_t color);
EditResult setClipName(Project& p, const std::string& clipId, const std::string& name);
EditResult setClipGain(Project& p, const std::string& clipId, float gainDb);
EditResult setFades(Project& p, const std::string& clipId, double fadeInBeats, double fadeOutBeats,
                    std::optional<FadeCurve> inCurve = std::nullopt, std::optional<FadeCurve> outCurve = std::nullopt);
// Crossfade between two audio clips on the same track: overlapping clips get
// matching fades over the overlap; butt-joined clips are overlapped by
// `lengthBeats` (using available source material) and faded.
EditResult crossfade(Project& p, const std::string& clipA, const std::string& clipB, double lengthBeats);
EditResult groupClips(Project& p, const std::vector<std::string>& clipIds);
EditResult ungroup(Project& p, const std::string& groupId);

// ---- selection ---------------------------------------------------------------
std::vector<std::string> clipsInRange(Project& p, const std::set<std::string>& trackIds, double startBeat, double endBeat);

// ---- markers, sections, loop ---------------------------------------------------
std::string addMarker(Project& p, double beat, const std::string& name);
bool removeMarker(Project& p, const std::string& id);
std::string addSection(Project& p, const std::string& name, const std::string& type, double startBeat, double endBeat);
bool removeSection(Project& p, const std::string& id);
void setLoop(Project& p, bool enabled, double startBeat, double endBeat);

} // namespace roy::arrange
