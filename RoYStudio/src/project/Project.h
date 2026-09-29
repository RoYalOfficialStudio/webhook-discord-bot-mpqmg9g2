#pragma once
// The RoY Studio project model (message thread only).
//
// Timeline positions are stored in beats (quarter notes, double). Audio
// source offsets are stored in seconds of the source file. The audio engine
// never reads this structure directly: ProjectRuntime compiles it into an
// immutable RenderGraph.
#include "audio/TempoMap.h"
#include "midi/Scale.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace roy {

using json = nlohmann::json;

constexpr int kProjectFormatVersion = 1;
constexpr const char* kProjectFileExtension = ".roy";

enum class FadeCurve : int { Linear = 0, EqualPower = 1, Exponential = 2, SCurve = 3 };

struct AudioAsset {
    std::string id;
    std::string path;          // relative to the project folder (or absolute for external refs)
    std::string originalName;
    std::string kind = "import"; // import | recording | render | stem | recovery | derived
    double sampleRate = 48000.0;
    int channels = 2;
    int64_t frames = 0;
    std::string sha256;
    std::string createdAt;
};

struct AudioClip {
    std::string id;
    std::string assetId;
    std::string name;
    uint32_t color = 0xD4AF37; // royal gold
    double startBeat = 0.0;
    double lengthBeats = 4.0;
    double sourceOffsetSec = 0.0; // where in the source this clip starts
    double stretch = 1.0;         // >1 = longer/slower (time-stretch, pitch preserved)
    double pitchSemitones = 0.0;  // clip transpose (pitch shift, rendered offline)
    float gainDb = 0.0f;
    bool muted = false;
    bool locked = false;
    bool reversed = false;
    std::string groupId;
    double fadeInBeats = 0.0;
    double fadeOutBeats = 0.0;
    FadeCurve fadeInCurve = FadeCurve::EqualPower;
    FadeCurve fadeOutCurve = FadeCurve::EqualPower;
    // Vocal A/B: after Pitch Guardian the clip remembers its untouched source (raw) and the
    // tuned render; assetId is whichever one plays. Neither file is ever modified.
    std::string rawAssetId;
    double rawOffsetSec = 0.0;
    std::string tunedAssetId;
    bool listeningOriginal() const { return !rawAssetId.empty() && assetId == rawAssetId; }
    double endBeat() const { return startBeat + lengthBeats; }
};

struct MidiNote {
    int pitch = 60;
    int velocity = 100;      // 1..127
    double startBeat = 0.0;  // relative to clip start
    double lengthBeats = 1.0;
    int channel = 0;
    bool muted = false;
    bool slide = false;      // 808 glide into this note
    double endBeat() const { return startBeat + lengthBeats; }
};

struct MidiClip {
    std::string id;
    std::string name;
    uint32_t color = 0xFF8C00;
    double startBeat = 0.0;
    double lengthBeats = 4.0;
    double loopLengthBeats = 0.0; // 0 = not looped, otherwise content repeats
    bool muted = false;
    bool locked = false;
    std::string groupId;
    std::vector<MidiNote> notes;
    double endBeat() const { return startBeat + lengthBeats; }
};

// ---- Beat Lab -----------------------------------------------------------------
struct Step {
    bool on = false;
    float velocity = 0.8f;     // 0..1
    float pan = 0.0f;          // -1..1
    float pitch = 0.0f;        // semitones
    float probability = 1.0f;  // 0..1
    float microTiming = 0.0f;  // fraction of a step, -0.5..0.5
    bool flam = false;
    int roll = 0;              // 0 = none, n = hits (ratchet / note repeat)
    int rollLength = 1;        // steps the roll is spread over (2 + roll 3 = 1/16 triplets)
};

struct PatternRow {
    std::string id;
    std::string name = "Kick";
    std::string voice = "kick"; // kick|snare|clap|closed_hat|open_hat|perc|rim|808|fx|sample
    std::string sampleAssetId; // when voice == "sample"
    int note = 36;              // note sent to the drum instrument
    bool muted = false;
    bool solo = false;
    float volume = 0.8f;
    float pan = 0.0f;
    float pitch = 0.0f;
    std::vector<Step> steps;
};

struct Pattern {
    std::string id;
    std::string name = "Pattern 1";
    int numSteps = 16;             // 16 / 32 / 64
    double stepLengthBeats = 0.25; // sixteenth notes
    float swing = 0.0f;            // 0..1 (0.5 = strong)
    std::string groove = "Straight"; // groove template (beat::grooveTemplates), applied on playback
    float grooveAmount = 1.0f;       // 0..1
    std::vector<PatternRow> rows;
    double lengthBeats() const { return numSteps * stepLengthBeats; }
};

struct PatternClip {
    std::string id;
    std::string patternId;
    double startBeat = 0.0;
    double lengthBeats = 4.0;
    bool muted = false;
    bool locked = false;
    uint32_t color = 0x2E8B57;
    double endBeat() const { return startBeat + lengthBeats; }
};

// ---- Recording ----------------------------------------------------------------
struct Take {
    std::string id;
    std::string assetId;
    std::string name;
    int lane = 0;
    double startBeat = 0.0;
    double lengthBeats = 0.0;
    std::string createdAt;
    int loopPass = 0;
};

struct CompSegment {
    double startBeat = 0.0;
    double endBeat = 0.0;
    std::string takeId;
};

// ---- Mixer ----------------------------------------------------------------------
struct PluginSlot {
    std::string id;
    std::string typeId;  // "roy.eq", "roy.compressor", "clap:<file>|<pluginId>", ...
    std::string name;
    bool bypass = false;
    json state = json::object(); // Processor::saveState()
    std::string sidechainChannelId;
};

struct Send {
    std::string id;
    std::string targetChannelId;
    float levelDb = -6.0f;
    bool preFader = false;
    bool enabled = true;
};

enum class ChannelKind : int { Track = 0, Bus = 1, Master = 2 };

struct MixerChannel {
    std::string id;
    std::string name;
    ChannelKind kind = ChannelKind::Track;
    float gainDb = 0.0f;
    float pan = 0.0f;       // -1..1
    float width = 1.0f;     // 0 = mono, 1 = original, 2 = extra wide
    bool mute = false;
    bool solo = false;
    bool soloSafe = false;  // never muted by other channels' solo (reference track, FX return feeds)
    bool phaseInvert = false;
    std::vector<PluginSlot> inserts;
    std::vector<Send> sends;
    std::string outputChannelId; // empty => master (for non-master channels)
    uint32_t color = 0xD4AF37;
};

struct AutomationPoint {
    double beat = 0.0;
    float value = 0.0f;
    int curve = 0;         // shape of the segment to the next point: 0 Linear, 1 Hold, 2 Smooth, 3 Bezier
    float tension = 0.0f;  // Bezier bend, -1..1
};

struct AutomationLane {
    std::string id;
    std::string channelId;
    std::string slotId;   // empty = channel strip parameter
    // "gain" | "pan" | "width" | "send:<sendId>" | "tempo" (master only) | processor param id
    std::string paramId;
    bool enabled = true;
    std::vector<AutomationPoint> points;
};

enum class TrackType : int { Audio = 0, Midi = 1, Beat = 2 };

struct Track {
    std::string id;
    std::string name;
    TrackType type = TrackType::Audio;
    uint32_t color = 0xD4AF37;
    std::string channelId;
    bool armed = false;
    bool monitor = false;
    int inputLeft = 0;   // device input channel index, -1 = none
    int inputRight = -1; // -1 = mono input
    float inputGainDb = 0.0f;
    bool locked = false;
    std::string role;    // "vocal", "adlib", "double", "harmony", "drums", "808", "music", ... (free text)
    std::optional<PluginSlot> instrument;
    std::vector<AudioClip> audioClips;
    std::vector<MidiClip> midiClips;
    std::vector<PatternClip> patternClips;
    std::vector<Take> takes;
    std::vector<CompSegment> comp;
};

struct Marker {
    std::string id;
    double beat = 0.0;
    std::string name;
    uint32_t color = 0xFFFFF0;
};

struct Section {
    std::string id;
    std::string name;
    std::string type = "verse"; // intro|verse|pre_hook|hook|bridge|outro|other
    double startBeat = 0.0;
    double endBeat = 16.0;
    uint32_t color = 0xFF8C00;
};

struct LoopRegion {
    bool enabled = false;
    double startBeat = 0.0;
    double endBeat = 16.0;
};

struct ProjectSettings {
    bool metronome = false;
    float metronomeGainDb = -6.0f;
    int countInBars = 0;
    double preRollBeats = 0.0;
    bool punchEnabled = false;
    double punchInBeat = 0.0;
    double punchOutBeat = 0.0;
    double snapBeats = 0.25;
    WrongNoteMode wrongNoteMode = WrongNoteMode::Off;
    int autosaveIntervalSec = 120;
    int backupRetention = 20;
    double neverLoseSeconds = 120.0;
};

struct Project {
    int formatVersion = kProjectFormatVersion;
    std::string id;
    std::string name = "Untitled";
    std::string author;
    std::string createdAt;
    std::string modifiedAt;
    std::string appVersion;
    double sampleRate = 48000.0;
    TempoMap tempo;
    Key key;
    ProjectSettings settings;
    LoopRegion loop;
    std::vector<Marker> markers;
    std::vector<Section> sections;
    std::vector<AudioAsset> assets;
    std::vector<Track> tracks;
    std::vector<MixerChannel> channels;
    std::vector<AutomationLane> automation;
    std::vector<Pattern> patterns;
    json presets = json::object();
    json producerMemory = json::object();
    json vocalSettings = json::object();  // per-track Pitch Guardian etc.: {trackId: {...}}
    json metadata = json::object();       // free metadata (genre, notes, ...)
    json unknown = json::object();        // unknown top-level keys from newer versions, preserved verbatim

    // ---- lookup helpers ---------------------------------------------------
    Track* findTrack(const std::string& id);
    const Track* findTrack(const std::string& id) const;
    MixerChannel* findChannel(const std::string& id);
    const MixerChannel* findChannel(const std::string& id) const;
    MixerChannel* master();
    const MixerChannel* master() const;
    AudioAsset* findAsset(const std::string& id);
    const AudioAsset* findAsset(const std::string& id) const;
    Pattern* findPattern(const std::string& id);
    const Pattern* findPattern(const std::string& id) const;
    AudioClip* findAudioClip(const std::string& clipId, Track** owner = nullptr);
    MidiClip* findMidiClip(const std::string& clipId, Track** owner = nullptr);
    PluginSlot* findSlot(const std::string& slotId, MixerChannel** owner = nullptr);
    double endBeat() const; // end of the last clip / section
};

// Creates a new empty project with master + default busses (VOCALS, DRUMS, MUSIC, FX).
Project makeNewProject(const std::string& name, double sampleRate = 48000.0, double bpm = 120.0);

// Adds a track with its own mixer channel routed to `outputChannelId` (or master).
Track& addTrack(Project& p, TrackType type, const std::string& name, const std::string& outputChannelId = {});
MixerChannel& addBus(Project& p, const std::string& name);
// Removes a track and its channel (clips/assets stay in the asset list: files are never deleted).
bool removeTrack(Project& p, const std::string& trackId);
// Deletes a bus: channels routed into it go to the bus's own output, sends into it are removed.
bool removeBus(Project& p, const std::string& channelId);
// True if adding a signal edge from -> to (output, send or sidechain) would close a loop.
bool routingWouldLoop(const Project& p, const std::string& from, const std::string& to);
// Assets the project can play or switch to: clips (incl. raw/tuned vocal versions), takes, sample rows.
std::set<std::string> referencedAssetIds(const Project& p);

// Default pattern with the standard Beat Lab rows.
Pattern makeDefaultPattern(const std::string& name, int numSteps = 16);

} // namespace roy
