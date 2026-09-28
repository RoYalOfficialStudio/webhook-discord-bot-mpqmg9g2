#pragma once
// RoY Studio GUI application state. Everything that changes the project goes
// through run(<command>) - the same command system the CLI and tests use - so
// every UI action is undoable, validated and logged.
#include "arrange/WaveformCache.h"
#include "browser/SamplePreview.h"
#include "audio/AudioEngine.h"
#include "audio/DeviceManager.h"
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"
#include "project/Session.h"
#include "record/Recorder.h"

#include <deque>
#include <filesystem>
#include <future>
#include <memory>
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
    bool recording() const { return recorder_.isRecording(); }
    void seekBeat(double beat);
    double positionBeats() const;
    std::string positionText() const;
    bool audioRunning() const { return device_.isRunning(); }
    std::string audioStatus() const { return audioStatus_; }
    bool restartAudio(const AudioDeviceConfig& cfg);
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
    uint32_t diskErrorsSeen_ = 0;
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
