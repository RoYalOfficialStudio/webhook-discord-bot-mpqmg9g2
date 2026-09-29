#pragma once
#include "midi/MidiInput.h"
// RoY Studio GUI application state. Everything that changes the project goes
// through run(<command>) - the same command system the CLI and tests use - so
// every UI action is undoable, validated and logged.
#include "arrange/WaveformCache.h"
#include "browser/SamplePreview.h"
#include "audio/AudioEngine.h"
#include "audio/DeviceManager.h"
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "mixer/ChannelPreset.h"
#include "arrange/BeatImport.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"
#include "project/Session.h"
#include "support/AppSettings.h"
#include "support/Diagnostics.h"
#include "support/SystemCheck.h"
#include "record/Recorder.h"

#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace roy::gui {

namespace fs = std::filesystem;

enum class Area { Playlist, Channels, PianoRoll, Mixer, Vocals, Beats, Plugins, Master, Project, Count };
const char* areaName(Area a);

struct AppOptions {
    std::string audioBackend = "auto"; // "null" for headless runs
    bool demo = false;                 // build a demo project on start
    fs::path demoFolder;
    bool interactive = true;           // false for self-test / screenshots / benchmarks: settings.json is neither read nor written
    bool forceFirstRun = false;        // --first-run: show the setup check even if it was done
    int setupStep = 0;                 // --setup-step N (screenshots of the wizard pages)
    bool openMusicSession = false;     // --music-session: open the FIRST REAL MUSIC SESSION window
};

struct UiLog {
    std::string text;
    int level = 2; // 0 info, 1 warn, 2 error
};

class App {
public:
    App();
    ~App();

    bool init(const AppOptions& o);
    void shutdown();
    void tick(); // once per frame: autosave, finished takes, crash events, meters

    // ---- project lifecycle ------------------------------------------------------
    bool newProject(const fs::path& parent, const std::string& name, double bpm);
    // safeMode: third-party plugins are not loaded (their state stays in the project)
    bool openProject(const fs::path& file, OpenMode mode = OpenMode::Normal, bool safeMode = false);
    bool save();
    void closeProject();
    bool hasProject() const { return project_ != nullptr; }
    bool dirty() const { return dirty_; }
    Project& project() { return *project_; }
    ProjectSession& session() { return session_; }
    ProjectRuntime& runtime() { return *runtime_; }
    AudioEngine& engine() { return engine_; }
    DeviceManager& device() { return device_; }
    UndoManager& undoManager() { return *undo_; }
    CommandRegistry& registry() { return registry_; }
    const json& lastResult() const { return lastResult_; }
    const std::string& lastError() const { return lastError_; }
    bool buildDemo(const fs::path& folder);
    // Drives the application layer end-to-end with the realtime device running
    // (demo -> play -> record -> vocal analysis -> mix check -> export -> undo/redo -> save -> reopen).
    // Prints one line per step; returns true if all steps pass.
    bool selfTest(const fs::path& folder);

    // ---- commands -----------------------------------------------------------------
    bool run(const std::string& id, const json& args = json::object());
    // Several commands as ONE undo step (all-or-nothing).
    bool runMacro(const std::string& name, const std::vector<std::pair<std::string, json>>& steps);
    bool undo();
    bool redo();

    // ---- transport ---------------------------------------------------------------
    void togglePlay();
    void stop();
    void toggleRecord();
    bool recording() const { return recorder_.isRecording() || midiRecording_; }
    // Live MIDI input (keyboard). Plays the selected track's instrument (or the armed MIDI track).
    midi::MidiInputManager* midiInput() { return midiIn_.get(); }
    void midiPanic();
    std::string liveMidiTrackName() const;
    // MIDI learn: the next controller moved (CC 0..119) is mapped to this target.
    // MIDI inputs switched on/off by the user (switched-off inputs are not reopened by hot-plug).
    void setMidiInputEnabled(const std::string& id, bool on);
    // Writes a diagnostics report (system, audio, MIDI, plugins, crash reports, log) for bug reports.
    std::string createDiagnosticsReport();
    void startMidiLearn(const std::string& channelId, const std::string& slotId, const std::string& paramId, const std::string& label);
    void cancelMidiLearn();
    bool midiLearning() const { return learn_.has_value(); }
    std::string midiLearnLabel() const { return learn_ ? learn_->label : std::string(); }
    void seekBeat(double beat);
    double positionBeats() const;
    std::string positionText() const;
    bool audioRunning() const { return device_.isRunning(); }
    std::string audioStatus() const { return audioStatus_; }
    bool restartAudio(const AudioDeviceConfig& cfg);
    // User-chosen audio change (menu / setup check): restart the device and remember it.
    bool changeAudio(const AudioDeviceConfig& cfg);
    // Setup check (first start, Help menu): device choice, test tone, input meter, MIDI, plugin scan.
    bool showFirstRun = false;
    int setupStep = 0;
    // Microphone test: 3 s from the input, then played back. 0 idle, 1 recording, 2 played back
    void testMicrophone();
    int micTestState() const { return micState_; }
    float micTestPeak() const { return micPeak_; }
    // callbacks over budget since the last audio change (dropout warning for small buffers)
    uint64_t overloadsSinceAudioChange() const { return engine_.stats().overloads - overloadBase_; }
    // sample rates to offer: 44.1 / 48 / 96 kHz; exclusive mode only the device's native ones
    std::vector<double> offeredSampleRates(std::vector<double>* native = nullptr) const;
    // SYSTEM CHECK
    bool showSystemCheck = false;
    std::vector<support::CheckItem> runSystemCheck();
    const std::vector<support::CheckItem>& lastSystemCheck() const { return lastCheck_; }
    // DIAGNOSTIC PACKAGE (zip) - returns the path ("" on error) and shows the folder
    std::string createDiagnosticPackage(bool showFolder = true);
    // FIRST REAL MUSIC SESSION guide
    bool showMusicSession = false;
    // export destination ("" = <project>/Exports)
    std::string exportFolder;
    fs::path currentExportFolder() const;
    void openFolder(const fs::path& folder);
    void finishFirstRun();
    void playTestTone();
    // Instrument preview (808 / bass / synth): plays one note on the track's instrument, also with
    // the transport stopped. Nothing is recorded or changed in the project.
    bool auditionNote(const std::string& trackId, int note, float velocity = 0.9f, double seconds = 0.8);
    bool auditionActive() const { return !auditionTrack_.empty(); }
    bool previewOnEdit = true; // 808 LAB / PIANO ROLL: play the sound after a change
    // ---- IMPORT BEAT (bought / downloaded MP3, WAV, FLAC ...) ----
    void pickAndImportBeat();                                     // native file dialog -> IMPORT BEAT window
    void beginImportBeat(const std::vector<fs::path>& files);      // first file in the window, the rest queued
    // bpm 0 / key "" keep the song's; beatGainDb = fader of the new beat track (headroom for vocals)
    bool importBeat(const fs::path& file, double bpm, const std::string& key, double beatGainDb = -6.0);
    void cancelImportBeat();
    bool showImportBeat = false;
    fs::path importBeatFile;
    const beatimport::BeatFileInfo* importBeatInfo(); // nullptr while the file is being analysed
    bool importNextQueued();
    // Files dropped from Windows Explorer onto the window (client pixel position).
    struct DroppedFiles {
        std::vector<fs::path> files;
        float x = 0, y = 0;
    };
    void filesDropped(std::vector<fs::path> files, float x, float y);
    std::optional<DroppedFiles>& droppedFiles() { return dropped_; } // the PLAYLIST may consume it
    void handleDroppedFiles();                                        // everything the UI did not take
    // LIVE VOCAL: monitoring + RoY VocalTune in the song key on this track (one undo step)
    bool liveVocal(const std::string& trackId);
    // ---- channel presets ("vocal chains"), stored in <user data>/Presets/Channel ----
    bool saveChannelPreset(const std::string& channelId, const std::string& name, bool overwrite);
    bool applyChannelPreset(const std::string& channelId, const json& preset);
    const std::vector<presets::PresetFile>& channelPresets(bool refresh = false);
    bool pluginScanRunning() const { return scanning_; }
    const AudioDeviceConfig& audioConfig() const { return audioCfg_; }

    // ---- browser preview ---------------------------------------------------------
    browser::Previewer& previewer() { return *previewer_; }
    browser::PreviewInfo previewFile(const fs::path& file);
    bool previewAuto = true;
    bool previewTempoSync = false;
    float previewVolume = 0.8f;
    browser::PreviewInfo lastPreview;
    // Import a dropped/double-clicked file as a project asset (copied into the project). Returns asset id.
    std::string importAsset(const fs::path& file);

    // ---- waveforms / assets --------------------------------------------------------
    std::shared_ptr<const WaveformCache> waveform(const std::string& assetId);

    // ---- plugins ---------------------------------------------------------------------
    plugins::PluginDatabase& pluginDb() { return pluginDb_; }
    void startPluginScan(bool force, bool retry);
    bool scanning() const { return scanning_; }
    const std::string& scanSummary() const { return scanSummary_; }
    void savePluginDb();
    std::vector<plugins::CrashEvent>& crashes() { return crashes_; }

    // ---- UI state ---------------------------------------------------------------------
    Area area = Area::Playlist;
    std::string selTrack, selClip, selMidiClip, selPattern, selChannel, selSlot;
    double pixelsPerBeat = 28.0;
    double scrollBeat = 0.0;
    double snapBeats = 0.25;
    bool showPalette = false;
    std::deque<UiLog> log;
    void message(int level, const std::string& text);
    json vocalResult = json::object();  // last vocal analysis result shown in VOCALS
    json masterResult = json::object(); // last mix/master analysis
    json checkResult = json::object();  // project assistant findings

private:
    void attachProject();
    void onProjectChanged(bool structural);
    bool offlineCommand(const std::string& id) const;

    AppOptions opt_;
    AudioEngine engine_;
    DeviceManager device_;
    AudioDeviceConfig audioCfg_;
    std::string audioStatus_;
    std::unique_ptr<ProjectRuntime> runtime_;
    std::unique_ptr<browser::Previewer> previewer_;
    std::unique_ptr<Project> project_;
    std::unique_ptr<UndoManager> undo_;
    std::unique_ptr<CommandContext> ctx_;
    CommandRegistry registry_;
    ProjectSession session_;
    Recorder recorder_;
    std::unique_ptr<midi::MidiInputManager> midiIn_;
    bool midiRecording_ = false;
    std::string midiRecordTrack_;
    std::vector<RecordedMidi> midiTake_;
    void updateLiveMidiTarget();
    void finishMidiRecording();
    std::future<beatimport::BeatFileInfo> importFuture_;
    std::optional<beatimport::BeatFileInfo> importInfo_;
    std::vector<fs::path> importQueue_;
    std::optional<DroppedFiles> dropped_;
    std::vector<presets::PresetFile> presetList_;
    bool presetListValid_ = false;
    std::string auditionTrack_;
    int auditionKey_ = -1;
    double auditionOff_ = 0, auditionRelease_ = 0;
    void pollAudition();
    void pollDialogs(); // results of the native file / folder windows (own thread)
    struct LearnTarget {
        std::string channelId, slotId, paramId, label;
    };
    std::optional<LearnTarget> learn_;
    std::string consumedSig_;
    void processMidiControls();
    void pollMidiDevices();
    support::AppSettings settings_;
    fs::path settingsFile_;
    void saveSettingsNow();
    int micState_ = 0;
    float micPeak_ = 0.0f;
    double micStart_ = 0.0;
    uint64_t overloadBase_ = 0;
    std::vector<support::CheckItem> lastCheck_;
    support::DiagnosticsInput diagnosticsInput();
    void pollMicTest();
    std::vector<std::string> midiUserOff_;
    std::set<std::string> midiFailReported_;
    double nextMidiRescan_ = 0;
    uint32_t diskErrorsSeen_ = 0;
    std::set<std::string> invalidWarned_;
    void pollAudioDevice();
    WaveformStore waveforms_;
    json lastResult_ = json::object();
    std::string lastError_;
    bool dirty_ = false;
    double lastAutosave_ = 0.0;

    plugins::PluginDatabase pluginDb_;
    fs::path pluginDbFile_;
    std::future<std::pair<plugins::PluginDatabase, plugins::ScanReport>> scanFuture_;
    bool scanning_ = false;
    std::string scanSummary_;
    std::vector<plugins::CrashEvent> crashes_;
};

} // namespace roy::gui
