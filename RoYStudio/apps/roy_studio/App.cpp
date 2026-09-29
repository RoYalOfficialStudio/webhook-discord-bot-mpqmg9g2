#include "App.h"
#include "midi/MidiLearn.h"

#include "core/CrashHandler.h"
#include "core/Files.h"
#include "core/Process.h"
#include "core/Log.h"
#include "support/Diagnostics.h"
#include "io/AudioFile.h"
#include "plugins/Sandbox.h"
#include "project/ProjectIO.h"
#include "record/Takes.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <cmath>
#include <format>

namespace roy::gui {

const char* areaName(Area a) {
    switch (a) {
    case Area::Playlist: return "PLAYLIST";
    case Area::Channels: return "CHANNELS";
    case Area::PianoRoll: return "PIANO ROLL";
    case Area::Mixer: return "MIXER";
    case Area::Vocals: return "VOCALS";
    case Area::Beats: return "BEATS";
    case Area::Plugins: return "PLUGINS";
    case Area::Master: return "MASTER";
    case Area::Project: return "PROJECT";
    default: return "?";
    }
}

namespace {
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

App::App() = default;

App::~App() { shutdown(); }

void App::message(int level, const std::string& text) {
    log.push_back({text, level});
    while (log.size() > 200) log.pop_front();
    if (level >= 2) log::error("ui", "{}", text);
    else if (level == 1) log::warn("ui", "{}", text);
    else log::info("ui", "{}", text);
}

bool App::init(const AppOptions& o) {
    opt_ = o;
    registerBuiltinProcessors();
    registerPluginProcessors();
    registerCoreCommands(registry_);
    const fs::path user = files::userDataDirectory();
    log::setFile((user / "roy_studio.log").string());
    crash::install((user / "CrashReports").string(), "roy_studio"); // RoY's own fatal errors -> report + minidump
    plugins::setCrashReportDirectory((user / "CrashReports").string());
    for (auto& r : crash::takeUnseenReports((user / "CrashReports").string()))
        message(2, "RoY Studio closed unexpectedly last time. A crash report was saved (" + fs::path(r).filename().string() +
                       "). Your project can be restored via RECOVER PROJECT; Help > Create diagnostics report collects the details for a bug report.");
    pluginDbFile_ = user / "plugins.json";
    std::string err;
    if (fs::exists(pluginDbFile_) && !pluginDb_.load(pluginDbFile_, &err)) {
        files::safeCopy(pluginDbFile_, files::uniquePath(pluginDbFile_.string() + ".corrupt"));
        message(1, err + " - a new plugin database will be created");
    }
    waveforms_.setCacheDirectory(user / "WaveformCache");

    settingsFile_ = user / "settings.json";
    if (o.interactive) {
        std::string note;
        settings_ = support::loadSettings(settingsFile_, &note);
        if (!note.empty()) message(1, note);
        audioCfg_ = settings_.audio;
        midiUserOff_ = settings_.midiInputsOff;
    }
    if (o.audioBackend != "auto" || !o.interactive) audioCfg_.backend = o.audioBackend; // command line wins (not saved)
    engine_.prepare(audioCfg_.sampleRate, audioCfg_.bufferSize);
    engine_.setWorkerThreads(AudioEngine::defaultWorkerThreads()); // multi-core mixing
    runtime_ = std::make_unique<ProjectRuntime>(engine_);
    previewer_ = std::make_unique<browser::Previewer>(engine_);
    if (!restartAudio(audioCfg_)) message(1, "audio device unavailable: " + audioStatus_ + " - running without audio output");
    auto hasDevice = [](const std::vector<AudioDeviceInfo>& list, const std::string& name) {
        return name.empty() || std::any_of(list.begin(), list.end(), [&](auto& d) { return d.name == name; });
    };
    if (device_.isRunning() && !hasDevice(device_.outputDevices(), audioCfg_.outputDevice))
        message(1, "saved output device '" + audioCfg_.outputDevice + "' not found - using the system default (Audio menu to choose)");
    if (device_.isRunning() && !hasDevice(device_.inputDevices(), audioCfg_.inputDevice))
        message(1, "saved input device '" + audioCfg_.inputDevice + "' not found - using the system default (Audio menu to choose)");
    showFirstRun = o.forceFirstRun || (o.interactive && !settings_.firstRunDone);
    setupStep = o.setupStep;
    showMusicSession = o.openMusicSession;
    engine_.setInputListener(&recorder_);
    recorder_.prepare(engine_.sampleRate(), engine_.maxBlockSize());
    // live MIDI: open every connected input (keyboards just work)
    midiIn_ = std::make_unique<midi::MidiInputManager>(engine_);
    for (auto& d : midiIn_->devices()) {
        if (std::find(midiUserOff_.begin(), midiUserOff_.end(), d.id) != midiUserOff_.end()) continue; // switched off by the user
        std::string err;
        if (!midiIn_->open(d.id, &err) && midiFailReported_.insert(d.id).second) message(1, "MIDI input " + d.name + ": " + err);
    }
    if (o.demo) return buildDemo(o.demoFolder.empty() ? fs::temp_directory_path() / "RoYStudioDemo" : o.demoFolder);
    return true;
}

bool App::restartAudio(const AudioDeviceConfig& cfg) {
    device_.stop();
    device_.close();
    audioCfg_ = cfg;
    std::string err;
    if (!device_.initialise(cfg.backend, &err) || !device_.open(cfg, engine_, &err) || !device_.start(&err)) {
        audioStatus_ = err.empty() ? "no device" : err;
        engine_.prepare(cfg.sampleRate, cfg.bufferSize);
        return false;
    }
    audioStatus_ = std::format("{}{} | {:.0f} Hz | {} samples | {:.1f} ms{}", device_.backendName(),
                               device_.backendName() == "WASAPI" ? (device_.exclusiveActive() ? " exclusive" : " shared") : "",
                               device_.actualSampleRate(), device_.actualBufferSize(),
                               1000.0 * device_.latencySamples() / device_.actualSampleRate(), device_.actualInputChannels() ? "" : " | no input");
    if (!device_.openNote().empty()) message(1, "audio: " + device_.openNote());
    overloadBase_ = engine_.stats().overloads;
    recorder_.prepare(engine_.sampleRate(), engine_.maxBlockSize());
    if (project_) runtime_->rebuild(*project_);
    return true;
}

void App::pollAudioDevice() {
    switch (device_.poll(nowSeconds())) {
    case DeviceManager::Health::Ok:
    case DeviceManager::Health::StillLost: break;
    case DeviceManager::Health::Lost: {
        // Keep everything that was recorded: finish open takes (they are added to the
        // project on this tick), stop the transport, then reconnect in the background.
        const bool wasRecording = recorder_.isRecording();
        if (wasRecording) recorder_.stopRecording();
        engine_.transport().stop();
        audioStatus_ = "DEVICE LOST - reconnecting...";
        message(2, "AUDIO DEVICE LOST: " + device_.lostReason() +
                       (wasRecording ? " - recording stopped, the take up to this point is kept" : "") +
                       ". RoY keeps running and reconnects automatically.");
        break;
    }
    case DeviceManager::Health::Reconnected:
        audioStatus_ = std::format("{} | {:.0f} Hz | {} samples | {:.1f} ms", device_.backendName(), device_.actualSampleRate(),
                                   device_.actualBufferSize(), 1000.0 * device_.latencySamples() / device_.actualSampleRate());
        audioCfg_ = device_.config();
        recorder_.prepare(engine_.sampleRate(), engine_.maxBlockSize());
        if (project_) runtime_->rebuild(*project_);
        message(0, "audio device reconnected: " + audioStatus_);
        break;
    }
}

void App::shutdown() {
    if (scanFuture_.valid()) scanFuture_.wait();
    if (midiIn_) midiIn_->closeAll();
    if (recorder_.isRecording()) recorder_.stopRecording();
    engine_.setInputListener(nullptr);
    device_.stop();
    device_.close();
    if (project_) closeProject();
    runtime_.reset();
}

// ------------------------------------------------------------------ project
void App::attachProject() {
    undo_ = std::make_unique<UndoManager>(*project_);
    undo_->onRestore = [this](const Project&) { onProjectChanged(true); };
    ctx_ = std::make_unique<CommandContext>(CommandContext{*project_, *undo_, runtime_.get(), session_.folder()});
    ctx_->changed = [this](bool structural) { onProjectChanged(structural); };
    runtime_->setProjectDirectory(session_.folder());
    runtime_->clearAssetCache();
    if (!runtime_->rebuild(*project_)) message(2, "project could not be compiled for playback");
    for (auto& w : runtime_->lastWarnings()) message(1, w);
    recorder_.setOutputFolder(session_.folder() / "Audio");
    selTrack = selClip = selMidiClip = selPattern = selChannel = selSlot = "";
    if (!project_->patterns.empty()) selPattern = project_->patterns.front().id;
    dirty_ = false;
    lastAutosave_ = nowSeconds();
}

void App::onProjectChanged(bool structural) {
    dirty_ = true;
    if (structural) {
        if (!runtime_->rebuild(*project_)) message(2, "graph rebuild failed - previous graph kept");
        waveforms_.retainOnly(referencedAssetIds(*project_)); // deleted takes/clips: peaks leave RAM too
    } else {
        runtime_->syncParams(*project_);
    }
    engine_.collectGarbage();
}

bool App::newProject(const fs::path& parent, const std::string& name, double bpm) {
    closeProject();
    runtime_->setSafeMode(false);
    auto p = std::make_unique<Project>(makeNewProject(name, engine_.sampleRate(), bpm));
    std::string err;
    if (!session_.create(parent, *p, &err)) {
        message(2, "cannot create project: " + err);
        return false;
    }
    project_ = std::move(p);
    attachProject();
    message(0, "new project " + session_.file().string());
    return true;
}

bool App::openProject(const fs::path& file, OpenMode mode, bool safeMode) {
    closeProject();
    runtime_->setSafeMode(safeMode);
    auto p = std::make_unique<Project>();
    std::string err;
    if (!session_.open(file, *p, mode, &err)) {
        message(2, "cannot open project: " + err);
        return false;
    }
    project_ = std::move(p);
    attachProject();
    message(safeMode ? 1 : 0, (safeMode ? "SAFE MODE (plugins not loaded): " : "") + std::string(session_.openedFromRecovery() ? "RECOVERED " : "opened ") + file.string());
    return true;
}

bool App::save() {
    if (!project_) return false;
    runtime_->captureProcessorStates(*project_);
    std::string err;
    if (!session_.save(*project_, &err)) {
        message(2, "SAVE FAILED: " + err + " (your project is still open - nothing was lost)");
        return false;
    }
    dirty_ = false;
    message(0, "saved " + session_.file().filename().string());
    return true;
}

void App::closeProject() {
    if (!project_) return;
    learn_.reset();
    if (midiIn_) midiIn_->setLearning(false);
    consumedSig_ = "-"; // re-sync the consumed controllers for the next project
    if (midiIn_) midiIn_->setConsumedControls({});
    if (engine_.transport().isPlaying()) stop();
    if (dirty_) {
        // Never lose work silently: keep a recovery snapshot of unsaved changes.
        runtime_->captureProcessorStates(*project_);
        session_.autosave(*project_, true);
    }
    runtime_->rebuild(Project{}); // detach processors of the old project
    session_.close();
    ctx_.reset();
    undo_.reset();
    project_.reset();
}

// ------------------------------------------------------------------ commands
bool App::offlineCommand(const std::string& id) const {
    static const char* ids[] = {"Export", "AnalyzeMix", "EnergyMap", "SeparateStems"};
    for (auto* s : ids)
        if (id == s) return true;
    return false;
}

bool App::run(const std::string& id, const json& args) {
    if (!project_) {
        message(1, "no project open");
        return false;
    }
    const bool pauseDevice = offlineCommand(id) && device_.isRunning();
    if (pauseDevice) device_.stop();
    const bool ok = registry_.execute(*ctx_, id, args);
    if (pauseDevice) {
        std::string err;
        device_.start(&err);
    }
    lastResult_ = ctx_->result;
    lastError_ = ctx_->error;
    if (!ok) message(1, id + ": " + ctx_->error);
    return ok;
}

bool App::runMacro(const std::string& name, const std::vector<std::pair<std::string, json>>& steps) {
    if (!project_) return false;
    const bool ok = registry_.executeMacro(*ctx_, name, steps);
    lastResult_ = ctx_->result;
    lastError_ = ctx_->error;
    if (!ok) message(1, name + ": " + ctx_->error);
    return ok;
}

bool App::undo() {
    if (!undo_ || !undo_->canUndo()) return false;
    const std::string n = undo_->undoName();
    const bool ok = undo_->undo();
    if (ok) message(0, "undo: " + n);
    return ok;
}

bool App::redo() {
    if (!undo_ || !undo_->canRedo()) return false;
    const std::string n = undo_->redoName();
    const bool ok = undo_->redo();
    if (ok) message(0, "redo: " + n);
    return ok;
}

// ------------------------------------------------------------------ transport
void App::togglePlay() {
    auto& t = engine_.transport();
    if (t.isPlaying()) {
        stop();
    } else {
        t.setCountInSamples(0); // count-in only before recordings
        t.play();
    }
}

void App::stop() {
    if (recorder_.isRecording()) recorder_.stopRecording();
    if (midiRecording_) finishMidiRecording();
    engine_.transport().stop();
}

void App::finishMidiRecording() {
    const int64_t end = engine_.transport().position();
    engine_.setLiveMidiRecording(false);
    midiRecording_ = false;
    engine_.drainRecordedMidi(midiTake_);
    json ev = json::array();
    for (auto& r : midiTake_) ev.push_back({r.timeline, r.msg.status, r.msg.data1, r.msg.data2});
    midiTake_.clear();
    if (ev.empty()) {
        message(1, "MIDI recording: nothing was played");
        return;
    }
    if (run("AddMidiRecording", {{"trackId", midiRecordTrack_}, {"events", ev}, {"sampleRate", engine_.sampleRate()}, {"endTimeline", end}}))
        message(0, std::format("MIDI take recorded: {} notes", lastResult_.value("notes", 0)));
}

bool App::auditionNote(const std::string& trackId, int note, float velocity, double seconds) {
    if (!project_ || !runtime_ || !midiIn_ || midiRecording_) return false; // never lands in a MIDI recording
    const Track* t = project_->findTrack(trackId);
    if (!t || !t->instrument) return false;
    if (auditionKey_ >= 0) { // previous preview note off first
        const uint8_t off[3] = {0x80, static_cast<uint8_t>(auditionKey_), 0};
        midiIn_->inject(off, 3);
    }
    auditionTrack_ = trackId;
    updateLiveMidiTarget(); // the engine sends all-notes-off to a previous target by itself
    auditionKey_ = std::clamp(note, 0, 127);
    const uint8_t on[3] = {0x90, static_cast<uint8_t>(auditionKey_), static_cast<uint8_t>(std::clamp(static_cast<int>(velocity * 127.0f), 1, 127))};
    midiIn_->inject(on, 3);
    auditionOff_ = nowSeconds() + std::clamp(seconds, 0.05, 10.0);
    auditionRelease_ = auditionOff_ + 0.5;
    return true;
}

void App::pollAudition() {
    if (auditionTrack_.empty() || !midiIn_) return;
    const double now = nowSeconds();
    if (auditionKey_ >= 0 && now >= auditionOff_) {
        const uint8_t off[3] = {0x80, static_cast<uint8_t>(auditionKey_), 0};
        midiIn_->inject(off, 3);
        auditionKey_ = -1;
    }
    // keep the preview track as live target a little longer so the note-off reaches it
    if (auditionKey_ < 0 && now >= auditionRelease_) auditionTrack_.clear();
}

void App::updateLiveMidiTarget() {
    if (!project_ || !runtime_) return;
    std::string target;
    if (midiRecording_) target = midiRecordTrack_;
    else if (!auditionTrack_.empty() && project_->findTrack(auditionTrack_)) target = auditionTrack_;
    else if (const Track* t = project_->findTrack(selTrack); t && t->instrument) target = t->id;
    else
        for (auto& t : project_->tracks)
            if (t.armed && t.instrument && target.empty()) target = t.id;
    if (target != runtime_->liveMidiTrack()) runtime_->setLiveMidiTrack(*project_, target);
}

std::string App::liveMidiTrackName() const {
    if (!project_ || !runtime_) return {};
    const Track* t = project_->findTrack(runtime_->liveMidiTrack());
    return t ? t->name : std::string();
}

std::string App::createDiagnosticsReport() {
    support::DiagnosticsInput in = diagnosticsInput();
    std::string err;
    const fs::path out = support::writeReport(in, {}, &err);
    if (out.empty()) {
        message(2, "diagnostics report not written: " + err);
        return {};
    }
    message(0, "diagnostics report written: " + out.string() + " - review it, then attach it to your bug report");
    return out.string();
}

std::string App::createDiagnosticPackage(bool showFolder) {
    std::string err;
    const auto checks = runSystemCheck();
    const fs::path out = support::writePackage(diagnosticsInput(), support::systemCheckMarkdown(checks), {}, &err);
    if (out.empty()) {
        message(2, "diagnostic package not written: " + err);
        return {};
    }
    message(0, "diagnostic package: " + out.string() + " - send this file (nothing is uploaded automatically)");
    if (showFolder) openFolder(out.parent_path());
    return out.string();
}

std::vector<support::CheckItem> App::runSystemCheck() {
    support::SystemCheckInput in;
    in.audioRunning = device_.isRunning();
    in.audioBackend = audioStatus_;
    in.sampleRate = device_.isRunning() ? device_.actualSampleRate() : audioCfg_.sampleRate;
    in.bufferSize = device_.isRunning() ? device_.actualBufferSize() : audioCfg_.bufferSize;
    in.outputDevices = static_cast<int>(device_.outputDevices().size());
    in.inputChannels = device_.isRunning() ? device_.actualInputChannels() : 0;
    in.overloads = overloadsSinceAudioChange();
    in.midiInputs = midiIn_ ? static_cast<int>(midiIn_->devices().size()) : 0;
    in.pluginHost = plugins::hostExecutable();
    in.projectsDir = files::defaultProjectsDirectory();
    lastCheck_ = support::runSystemCheck(in);
    log::info("app", "system check: {}", support::overall(lastCheck_));
    return lastCheck_;
}

std::vector<double> App::offeredSampleRates(std::vector<double>* nativeOut) const {
    const std::vector<double> native = device_.nativeSampleRates(audioCfg_.outputDevice, false);
    if (nativeOut) *nativeOut = native;
    std::vector<double> out;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        const bool isNative = std::find(native.begin(), native.end(), sr) != native.end();
        if (!audioCfg_.exclusive || native.empty() || isNative) out.push_back(sr); // shared mode converts any rate
    }
    return out;
}

fs::path App::currentExportFolder() const {
    if (!exportFolder.empty()) return exportFolder;
    return session_.folder().empty() ? files::defaultProjectsDirectory() : session_.folder() / "Exports";
}

void App::openFolder(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (!openInFileBrowser(folder.string())) message(1, "cannot open the folder: " + folder.string());
}

void App::testMicrophone() {
    if (!device_.isRunning() || device_.actualInputChannels() == 0) {
        message(2, "no audio input is open - choose an input device (Windows: Settings > Privacy > Microphone must allow desktop apps)");
        return;
    }
    if (!engine_.startInputCapture(3.0)) return;
    micState_ = 1;
    micPeak_ = 0.0f;
    micStart_ = nowSeconds();
    message(0, "microphone test: speak now (3 seconds)...");
}

void App::pollMicTest() {
    if (micState_ != 1) return;
    std::vector<float> l, r;
    if (engine_.takeInputCapture(l, r)) {
        for (size_t i = 0; i < l.size(); ++i) micPeak_ = std::max({micPeak_, std::fabs(l[i]), std::fabs(r[i])});
        const fs::path f = files::userDataDirectory() / "microphone_test.wav";
        std::error_code ec;
        fs::remove(f, ec); // only our own test file
        std::string err;
        if (writeWavFile(f, {l, r}, engine_.sampleRate(), SampleFormat::Pcm24, false, &err)) previewFile(f);
        micState_ = 2;
        const float db = 20.0f * std::log10(std::max(1e-6f, micPeak_));
        message(micPeak_ < 0.001f ? 2 : 0, micPeak_ < 0.001f ? "microphone test: NO SIGNAL - check the input device, cable, gain and Windows microphone privacy"
                                                             : std::format("microphone test: peak {:.0f} dBFS - playing it back now", db));
    } else if (nowSeconds() - micStart_ > 8.0) {
        engine_.cancelInputCapture();
        micState_ = 0;
        message(2, "microphone test: no audio arrived from the device (is the audio device running?)");
    }
}

support::DiagnosticsInput App::diagnosticsInput() {
    support::DiagnosticsInput in;
    in.userDataDir = files::userDataDirectory();
    in.audioLines.push_back("backend: " + (device_.isRunning() ? device_.backendName() : std::string("not running")) + " | status: " + audioStatus_);
    in.audioLines.push_back(std::format("settings: {:.0f} Hz, buffer {} samples, output '{}', input '{}'", audioCfg_.sampleRate, audioCfg_.bufferSize,
                                        audioCfg_.outputDevice.empty() ? "default" : audioCfg_.outputDevice,
                                        audioCfg_.inputDevice.empty() ? "default" : audioCfg_.inputDevice));
    const auto st = engine_.stats();
    in.audioLines.push_back(std::format("engine: {} callbacks, {} overloads (possible dropouts), peak load {:.0f} %, {} worker threads", st.callbacks,
                                        st.overloads, st.peakCpuLoad * 100.0, engine_.workerThreads()));
    for (auto& d : device_.outputDevices()) in.audioLines.push_back("output device: " + d.name + (d.isDefault ? " (default)" : ""));
    for (auto& d : device_.inputDevices()) in.audioLines.push_back("input device: " + d.name + (d.isDefault ? " (default)" : ""));
    if (midiIn_) {
        for (auto& d : midiIn_->devices()) in.midiLines.push_back(d.name + (midiIn_->isOpen(d.id) ? " (open)" : " (off)"));
        in.midiLines.push_back(std::format("messages received: {}, dropped by the engine: {}", midiIn_->messageCount(), engine_.liveMidiDropped()));
    }
    if (project_) { // counts only - no names or content
        size_t plugins = 0;
        for (auto& c : project_->channels) plugins += c.inserts.size();
        in.sessionLines.push_back(std::format("project open: {} tracks, {} mixer channels, {} effect slots, {} assets, safe mode {}", project_->tracks.size(),
                                              project_->channels.size(), plugins, project_->assets.size(), runtime_->safeMode() ? "on" : "off"));
        in.sessionLines.push_back(std::format("plugin crashes this session: {}", crashes_.size()));
    }
    in.sessionLines.push_back(std::string("portable mode: ") + (files::portableMode() ? "yes" : "no"));
    return in;
}

void App::saveSettingsNow() {
    if (!opt_.interactive) return;
    settings_.midiInputsOff = midiUserOff_;
    std::string err;
    if (!support::saveSettings(settingsFile_, settings_, &err)) message(1, "settings not saved: " + err);
}

bool App::changeAudio(const AudioDeviceConfig& cfg) {
    const bool ok = restartAudio(cfg);
    const std::string keepBackend = settings_.audio.backend;
    settings_.audio = cfg;
    if (opt_.audioBackend != "auto") settings_.audio.backend = keepBackend; // a command-line backend is not remembered
    saveSettingsNow();
    if (!ok) message(2, "audio device could not start: " + audioStatus_);
    return ok;
}

void App::finishFirstRun() {
    showFirstRun = false;
    settings_.firstRunDone = true;
    saveSettingsNow();
}

void App::playTestTone() {
    // 1 s 440 Hz at -12 dBFS with short fades, generated once into the user folder
    const fs::path f = files::userDataDirectory() / "test_tone_440Hz.wav";
    if (!fs::exists(f)) {
        const double sr = 48000.0;
        std::vector<float> v(static_cast<size_t>(sr));
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = static_cast<double>(i) / sr;
            const double fade = std::min({1.0, t / 0.02, (1.0 - t) / 0.02});
            v[i] = static_cast<float>(0.25 * fade * std::sin(6.283185307179586 * 440.0 * t));
        }
        std::string err;
        if (!writeWavFile(f, {v, v}, sr, SampleFormat::Pcm24, false, &err)) {
            message(2, "test tone: " + err);
            return;
        }
    }
    previewFile(f);
}

void App::setMidiInputEnabled(const std::string& id, bool on) {
    if (!midiIn_) return;
    std::erase(midiUserOff_, id);
    if (!on) {
        midiUserOff_.push_back(id);
        midiIn_->close(id);
        saveSettingsNow();
        return;
    }
    saveSettingsNow();
    std::string err;
    midiFailReported_.erase(id);
    if (!midiIn_->open(id, &err)) message(2, err);
}

void App::pollMidiDevices() {
    if (!midiIn_) return;
    const double now = nowSeconds();
    if (now < nextMidiRescan_) return;
    nextMidiRescan_ = now + 2.0;
    const auto r = midiIn_->rescan(midiUserOff_);
    for (auto& d : r.added) {
        midiFailReported_.erase(d.id);
        message(0, "MIDI input connected: " + d.name);
    }
    for (auto& id : r.removed) {
        for (uint8_t ch = 0; ch < 16; ++ch) { // device gone mid-note: no hanging notes (serialised input path)
            const uint8_t off[3] = {static_cast<uint8_t>(0xB0 | ch), 123, 0};
            midiIn_->inject(off, 3);
        }
        message(1, "MIDI input disconnected: " + (id.rfind("winmm:", 0) == 0 ? id.substr(6) : id));
    }
    for (auto& f : r.failed)
        if (midiFailReported_.insert(f.substr(0, f.find(':'))).second) message(1, "MIDI input " + f);
}

void App::startMidiLearn(const std::string& channelId, const std::string& slotId, const std::string& paramId, const std::string& label) {
    if (!midiIn_) return;
    learn_ = LearnTarget{channelId, slotId, paramId, label};
    midiIn_->drainControlChanges(); // only a controller moved from now on counts
    midiIn_->setLearning(true);
    message(0, "MIDI LEARN: move a knob or fader on your controller for " + label + " (Esc cancels)");
}

void App::cancelMidiLearn() {
    if (!learn_) return;
    learn_.reset();
    if (midiIn_) midiIn_->setLearning(false);
    message(0, "MIDI learn cancelled");
}

void App::processMidiControls() {
    if (!midiIn_ || !project_) return;
    auto ccs = midiIn_->drainControlChanges();
    if (learn_ && !ccs.empty()) {
        const auto c = ccs.front();
        const LearnTarget t = *learn_;
        learn_.reset();
        midiIn_->setLearning(false);
        if (run("AddMidiMapping", {{"cc", c.cc}, {"channel", c.channel}, {"channelId", t.channelId}, {"slotId", t.slotId}, {"paramId", t.paramId}}))
            message(0, std::format("MIDI LEARN: CC {} (channel {}) now controls {}", c.cc, c.channel + 1, t.label));
        ccs.clear(); // the learning move itself does not change the value
    }
    if (!ccs.empty() && midi::applyControls(*project_, runtime_.get(), ccs) > 0) dirty_ = true;
    // mapped controllers are consumed by the input (not sent to the instrument)
    std::string sig;
    std::vector<std::pair<int, int>> list;
    for (auto& m : project_->midiMappings) {
        list.push_back({m.channel, m.cc});
        sig += std::format("{}:{},", m.channel, m.cc);
    }
    if (sig != consumedSig_) {
        midiIn_->setConsumedControls(list);
        consumedSig_ = sig;
    }
}

void App::midiPanic() {
    if (!midiIn_) return;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint8_t msg[6] = {static_cast<uint8_t>(0xB0 | ch), 64, 0, static_cast<uint8_t>(0xB0 | ch), 123, 0}; // pedal up + all notes off
        midiIn_->inject(msg, sizeof(msg));
    }
    message(0, "MIDI panic: all notes off");
}

void App::toggleRecord() {
    if (!project_) return;
    if (recorder_.isRecording() || midiRecording_) {
        stop();
        return;
    }
    bool audioArmed = false;
    std::string midiArmed;
    for (auto& t : project_->tracks) {
        audioArmed |= t.armed && t.type == TrackType::Audio;
        if (t.armed && t.type == TrackType::Midi && midiArmed.empty()) midiArmed = t.id;
    }
    if (!audioArmed && midiArmed.empty()) {
        message(1, "arm a track first (R button in the playlist)");
        return;
    }
    if (audioArmed) {
        recorder_.setTracks(takes::recordConfig(*project_));
        recorder_.setOutputFolder(session_.folder() / "Audio");
        std::string err;
        if (!recorder_.startRecording(&err)) {
            message(2, "cannot record: " + err);
            return;
        }
    }
    if (!midiArmed.empty()) { // live MIDI goes to the armed MIDI track and is captured
        midiRecordTrack_ = midiArmed;
        midiTake_.clear();
        midiRecording_ = true;
        updateLiveMidiTarget();
        engine_.setLiveMidiRecording(true);
    }
    // count-in (Project > metronome settings): clicks for N bars, then recording starts
    const int bars = project_->settings.countInBars;
    int64_t countIn = 0;
    if (bars > 0) {
        const double beat = positionBeats();
        const double secPerBeat = project_->tempo.beatToSeconds(beat + 1.0) - project_->tempo.beatToSeconds(beat);
        const int num = project_->tempo.signatureAtBar(0).numerator;
        countIn = static_cast<int64_t>(std::llround(bars * num * secPerBeat * engine_.sampleRate()));
    }
    engine_.transport().setCountInSamples(countIn);
    engine_.transport().play();
    message(0, countIn > 0 ? std::format("count-in {} bar(s), then recording...", bars) : std::string("recording..."));
}

void App::seekBeat(double beat) {
    if (!project_) return;
    engine_.transport().seek(static_cast<int64_t>(std::llround(project_->tempo.beatToSeconds(std::max(0.0, beat)) * engine_.sampleRate())));
}

double App::positionBeats() const {
    if (!project_) return 0.0;
    return project_->tempo.secondsToBeat(static_cast<double>(engine_.transport().position()) / engine_.sampleRate());
}

std::string App::positionText() const {
    const double b = positionBeats();
    const BarBeat bb = project_ ? project_->tempo.beatToBarBeat(b) : BarBeat{static_cast<int>(b / 4), std::fmod(b, 4.0)};
    const double secs = static_cast<double>(engine_.transport().position()) / engine_.sampleRate();
    return std::format("{:3d}.{}.{:02d}   {:02d}:{:06.3f}", bb.bar + 1, static_cast<int>(bb.beatInBar) + 1,
                       static_cast<int>(std::fmod(bb.beatInBar, 1.0) * 100), static_cast<int>(secs / 60), std::fmod(secs, 60.0));
}

// ------------------------------------------------------------------ periodic
void App::tick() {
    log::flushAudioEvents();
    engine_.collectGarbage();
    if (previewer_) previewer_->collect();
    pollAudioDevice();
    pollMidiDevices();
    pollMicTest();
    if (!project_) return;
    pollAudition();
    updateLiveMidiTarget();
    processMidiControls();
    engine_.metronome().setEnabled(project_->settings.metronome); // CLICK button / project setting
    engine_.metronome().setGainDb(project_->settings.metronomeGainDb);
    if (midiRecording_) engine_.drainRecordedMidi(midiTake_); // keep the engine queue short
    if (recorder_.diskErrors() != diskErrorsSeen_) {
        diskErrorsSeen_ = recorder_.diskErrors();
        message(2, "DISK WRITE FAILED: " + recorder_.lastDiskError());
    }
    // finished takes -> project (one undo step per take)
    for (auto& take : recorder_.collectFinishedTakes()) {
        undo_->begin("Record Take");
        const std::string id = takes::addRecordedTake(*project_, take, session_.folder());
        if (id.empty()) {
            undo_->cancel();
            message(2, "take could not be added (the file is kept in the Audio folder)");
        } else {
            undo_->end();
            onProjectChanged(true);
            message(0, std::format("take recorded: {:.1f} s", static_cast<double>(take.frames) / engine_.sampleRate()));
        }
    }
    // plugins that output NaN/Inf (silenced by the sandbox): tell the user once per plugin
    if (runtime_)
        for (auto& [slotId, proc] : runtime_->allProcessors())
            if (auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get()); sp && sp->invalidSamples() > 0 && invalidWarned_.insert(slotId).second)
                message(2, "PLUGIN OUTPUT INVALID: " + sp->displayName() + " produced NaN/Inf samples - RoY replaced them with silence. "
                               "The plugin has a bug; consider disabling or replacing it.");
    // plugin crash events
    for (auto& e : plugins::takeCrashEvents()) {
        message(2, "PLUGIN CRASHED: " + e.pluginName + " - " + e.reason + " (bypassed, project continues; restart it in PLUGINS)");
        crashes_.push_back(e);
    }
    // background plugin scan finished?
    if (scanning_ && scanFuture_.valid() && scanFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [db, rep] = scanFuture_.get();
        pluginDb_ = std::move(db);
        scanning_ = false;
        scanSummary_ = std::format("{} modules, {} plugins OK, {} failed, {} crashed, {} timeouts, {} VST3 detected (unsupported), {:.1f} s",
                                   rep.modulesFound, rep.pluginsOk, rep.failed, rep.crashed, rep.timeouts, rep.unsupported, rep.seconds);
        savePluginDb();
        message(0, "plugin scan: " + scanSummary_);
    }
    // autosave every 60 s while there are unsaved changes (never touches the project file)
    const double now = nowSeconds();
    if (dirty_ && now - lastAutosave_ > 60.0) {
        runtime_->captureProcessorStates(*project_);
        if (session_.autosave(*project_)) lastAutosave_ = now;
    }
}

browser::PreviewInfo App::previewFile(const fs::path& file) {
    browser::PreviewOptions o;
    o.gain = previewVolume;
    o.tempo = previewTempoSync ? browser::TempoMode::Project : browser::TempoMode::Original;
    o.projectBpm = project_ ? project_->tempo.tempoAt(positionBeats()) : 120.0;
    lastPreview = previewer_->preview(file, o);
    if (!lastPreview.ok) message(1, "preview: " + lastPreview.error);
    if (!device_.isRunning()) message(1, "preview: audio device is not running");
    return lastPreview;
}

std::string App::importAsset(const fs::path& file) {
    if (!run("ImportAudio", {{"path", file.string()}, {"copy", true}})) return {};
    return lastResult_.value("assetId", "");
}

std::shared_ptr<const WaveformCache> App::waveform(const std::string& assetId) {
    if (!project_) return nullptr;
    auto data = runtime_->asset(*project_, assetId);
    if (!data) return nullptr;
    return waveforms_.get(assetId, *data);
}

// ------------------------------------------------------------------ plugins
void App::startPluginScan(bool force, bool retry) {
    if (scanning_) return;
    scanning_ = true;
    plugins::ScanOptions o;
    o.paths = plugins::defaultPluginPaths();
    o.force = force;
    o.retryQuarantined = retry;
    plugins::PluginDatabase copy = pluginDb_;
    scanFuture_ = std::async(std::launch::async, [copy, o]() mutable {
        auto rep = plugins::scanPlugins(copy, o);
        return std::make_pair(std::move(copy), rep);
    });
    message(0, "scanning plugins (out of process)...");
}

void App::savePluginDb() {
    std::string err;
    if (!pluginDb_.save(pluginDbFile_, &err)) message(2, "plugin database not saved: " + err);
}

// ------------------------------------------------------------------ demo project
bool App::buildDemo(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path existing = folder / "RoY Demo" / "RoY Demo.roy";
    if (fs::exists(existing)) fs::rename(folder / "RoY Demo", files::uniquePath(folder / "RoY Demo (old)"), ec); // keep, never delete
    if (!newProject(folder, "RoY Demo", 142.0)) return false;
    run("SetKey", {{"key", "A Minor"}});
    auto bus = [&](const std::string& n) {
        for (auto& c : project_->channels)
            if (c.name == n) return c.id;
        return std::string();
    };
    // a synthetic "vocal" phrase (MOCK audio for the demo, generated here)
    {
        const double sr = 48000.0;
        std::vector<float> v(static_cast<size_t>(sr * 6.5));
        const double notes[] = {57, 60, 64, 62, 60, 57, 55, 57};
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = static_cast<double>(i) / sr;
            const int n = static_cast<int>(t / 0.8);
            const double local = std::fmod(t, 0.8);
            if (n >= 8 || local > 0.65) continue;
            const double f = 440.0 * std::pow(2.0, (notes[n] - 69) / 12.0) * (1.0 + 0.004 * std::sin(6.2831853 * 5.5 * t));
            const double env = std::min(1.0, local / 0.03) * std::min(1.0, (0.65 - local) / 0.08);
            double s = 0;
            for (int h = 1; h <= 12; ++h) {
                const double fh = f * h;
                const double formant = std::exp(-std::pow((fh - 700) / 150, 2)) + 0.6 * std::exp(-std::pow((fh - 1200) / 180, 2)) + 0.05;
                s += formant * std::sin(6.2831853 * fh * t) / h;
            }
            v[i] = static_cast<float>(0.35 * env * s);
        }
        std::string err;
        writeWavFile(session_.folder() / "Audio" / "demo_vocal.wav", {v}, sr, SampleFormat::Pcm24, false, &err);
    }
    run("AddTrack", {{"type", "audio"}, {"name", "Lead Vocal"}, {"output", bus("VOCALS")}, {"role", "vocal"}});
    const std::string vocal = lastResult_.value("id", "");
    const std::string vocalCh = lastResult_.value("channelId", "");
    run("ImportAudio", {{"path", (session_.folder() / "Audio" / "demo_vocal.wav").string()}, {"trackId", vocal}, {"startBeat", 8.0}, {"copy", false}});
    run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.eq"}, {"name", "RoY EQ"}});
    run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.compressor"}, {"name", "RoY Comp"}});
    run("AddInsert", {{"channelId", vocalCh}, {"typeId", "roy.deesser"}, {"name", "RoY De-Esser"}});

    run("AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"output", bus("DRUMS")}, {"role", "drums"}});
    const std::string drums = lastResult_.value("id", "");
    run("AddPattern", {{"name", "Trap Beat"}, {"steps", 16}});
    const std::string pat = lastResult_.value("id", "");
    selPattern = pat;
    run("SetRowPattern", {{"patternId", pat}, {"voice", "kick"}, {"text", "X.....x...x....."}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "snare"}, {"text", "....X.......X..."}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "clap"}, {"text", "....x.......x..."}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "closed_hat"}, {"text", "xoxoxoxoxoxoxxxx"}});
    run("SetRowPattern", {{"patternId", pat}, {"voice", "open_hat"}, {"text", "..............x."}});
    run("AddPatternClip", {{"trackId", drums}, {"patternId", pat}, {"startBeat", 0.0}, {"lengthBeats", 32.0}});

    run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"output", bus("DRUMS")}, {"instrument", "roy.808"}, {"role", "808"}});
    const std::string bass = lastResult_.value("id", "");
    run("AddMidiClip", {{"trackId", bass}, {"startBeat", 0.0}, {"lengthBeats", 16.0}, {"name", "808 Line"}});
    const std::string bassClip = lastResult_.value("id", "");
    run("AddNote", {{"clipId", bassClip}, {"pitch", 33}, {"startBeat", 0.0}, {"lengthBeats", 1.5}});
    run("AddNote", {{"clipId", bassClip}, {"pitch", 33}, {"startBeat", 2.5}, {"lengthBeats", 1.0}});
    run("AddNote", {{"clipId", bassClip}, {"pitch", 36}, {"startBeat", 4.0}, {"lengthBeats", 1.5}, {"slide", true}});
    run("AddNote", {{"clipId", bassClip}, {"pitch", 31}, {"startBeat", 8.0}, {"lengthBeats", 2.0}});
    run("AddNote", {{"clipId", bassClip}, {"pitch", 29}, {"startBeat", 12.0}, {"lengthBeats", 3.0}});
    run("DuplicateClip", {{"clipId", bassClip}, {"startBeat", 16.0}});

    run("AddTrack", {{"type", "midi"}, {"name", "Keys"}, {"output", bus("MUSIC")}, {"role", "music"}});
    const std::string keys = lastResult_.value("id", "");
    run("AddMidiClip", {{"trackId", keys}, {"startBeat", 0.0}, {"lengthBeats", 16.0}, {"name", "Chords"}});
    const std::string keysClip = lastResult_.value("id", "");
    const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
    for (int c = 0; c < 4; ++c)
        for (int n : chords[c]) run("AddNote", {{"clipId", keysClip}, {"pitch", n}, {"startBeat", c * 4.0}, {"lengthBeats", 3.75}, {"velocity", 90}});
    run("DuplicateClip", {{"clipId", keysClip}, {"startBeat", 16.0}});
    run("AddSection", {{"name", "Intro"}, {"type", "intro"}, {"startBeat", 0.0}, {"endBeat", 8.0}});
    run("AddSection", {{"name", "Hook"}, {"type", "hook"}, {"startBeat", 8.0}, {"endBeat", 24.0}});
    run("AddSection", {{"name", "Verse"}, {"type", "verse"}, {"startBeat", 24.0}, {"endBeat", 32.0}});
    run("AddMarker", {{"name", "Drop"}, {"beat", 8.0}});
    run("CreateMasterChain", {{"preset", "streaming"}});
    selTrack = vocal;
    selMidiClip = keysClip;
    if (auto* t = project_->findTrack(vocal); t && !t->audioClips.empty()) selClip = t->audioClips[0].id;
    selChannel = vocalCh;
    save();
    return true;
}

// ------------------------------------------------------------------ self test
bool App::selfTest(const fs::path& folder) {
    int failed = 0;
    std::error_code ec;
    fs::create_directories(folder, ec);
    std::string report = std::format("RoY Studio {} self test {}\n", ROY_VERSION_STRING, files::nowIso8601());
    auto step = [&](const char* name, bool ok, const std::string& detail = {}) {
        const std::string line = std::format("[{}] {}{}{}\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " - ", detail);
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
        report += line;
        files::atomicWrite(folder / "selftest_report.txt", report);
        if (!ok) ++failed;
        return ok;
    };
    auto pump = [&](double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        tick();
    };
    step("audio device running", device_.isRunning(), audioStatus_);
    if (!step("demo project", buildDemo(folder))) return false;
    const fs::path file = session_.file();
    // playback in real time; the master meter must show signal while playing
    seekBeat(0);
    togglePlay();
    float meterPeak = 0.0f;
    auto master = runtime_->channelParams(project_->master()->id);
    for (int i = 0; i < 100; ++i) {
        pump(0.01);
        if (master) meterPeak = std::max(meterPeak, master->peakL.load());
    }
    const double pos = positionBeats();
    stop();
    step("realtime playback advances", pos > 1.0, std::format("{:.2f} beats after 1 s", pos));
    step("master meter shows signal", meterPeak > 0.001f, std::format("peak {:.3f}", meterPeak));
    // recording (null device input = silence, but the full path runs)
    std::string vocal;
    for (auto& t : project_->tracks)
        if (t.role == "vocal") vocal = t.id;
    run("ArmTrack", {{"trackId", vocal}, {"armed", true}});
    const size_t takesBefore = project_->findTrack(vocal)->takes.size();
    seekBeat(32);
    toggleRecord();
    pump(0.8);
    toggleRecord();
    pump(0.3);
    step("record take (realtime)", project_->findTrack(vocal)->takes.size() == takesBefore + 1);
    // metronome (CLICK) and count-in reach the engine
    {
        run("SetMetronome", {{"enabled", true}, {"countInBars", 1}});
        const uint64_t clicks0 = engine_.metronome().clicksTriggered();
        seekBeat(0);
        togglePlay();
        pump(0.7);
        stop();
        const uint64_t clicks1 = engine_.metronome().clicksTriggered();
        step("metronome audible (CLICK)", clicks1 > clicks0, std::format("{} clicks", clicks1 - clicks0));
        seekBeat(40);
        toggleRecord();
        pump(0.15);
        const bool countingIn = engine_.transport().isCountingIn();
        toggleRecord();
        pump(0.3);
        step("count-in before recording", countingIn);
        run("SetMetronome", {{"enabled", false}, {"countInBars", 0}});
    }
    run("ArmTrack", {{"trackId", vocal}, {"armed", false}});
    // 808 preview (transport stopped): the note must reach the master and end by itself
    {
        std::string bass;
        for (auto& t : project_->tracks)
            if (t.instrument && t.instrument->typeId == "roy.808") bass = t.id;
        if (step("808 track for preview", !bass.empty())) {
            stop();
            pump(0.3);
            if (master) master->peakL.store(0.0f);
            float peak = 0.0f;
            const bool started = auditionNote(bass, 36, 1.0f, 0.4);
            for (int i = 0; i < 40; ++i) {
                pump(0.01);
                if (master) peak = std::max(peak, master->peakL.exchange(0.0f));
            }
            step("808 preview audible (transport stopped)", started && peak > 0.001f, std::format("peak {:.3f}", peak));
            pump(1.0);
            step("808 preview ends by itself", !auditionActive());
        }
    }
    // vocal lab
    std::string clip;
    for (auto& c : project_->findTrack(vocal)->audioClips) clip = c.id;
    {
        const bool ok = run("PitchAnalysis", {{"clipId", clip}});
        const size_t notes = ok ? lastResult_.value("notes", json::array()).size() : 0;
        step("pitch analysis", ok && notes >= 4, std::format("{} notes", notes));
    }
    step("pitch guardian (lock + off-key filter)", run("PitchGuardian", {{"clipId", clip}, {"mode", "lock"}, {"offKeyFilter", true}}));
    step("vocal doctor", run("VocalDoctor", {{"clipId", clip}}));
    // mix + master (device paused automatically for offline renders)
    step("mix analysis", run("AnalyzeMix", {{"startBeat", 0.0}, {"endBeat", 32.0}}) && lastResult_.contains("master"));
    const bool exported = run("Export", {{"format", "wav"}, {"name", "selftest"}});
    std::string out = exported ? lastResult_["files"][0].value("path", "") : "";
    step("export wav", exported && fs::exists(out), out);
    step("audio device resumed after export", device_.isRunning());
    // MIDI learn: learn mode -> a controller move maps it -> further moves drive the master volume
    if (midiIn_ && project_->master()) {
        const std::string masterId = project_->master()->id;
        startMidiLearn(masterId, "", "gain", "Master · Volume");
        const uint8_t learnMove[3] = {0xB0, 102, 40}, fullUp[3] = {0xB0, 102, 127};
        midiIn_->inject(learnMove, 3);
        tick();
        const MidiMapping* m = midi::findMappingForTarget(*project_, masterId, "", "gain");
        midiIn_->inject(fullUp, 3);
        tick();
        const float g = project_->master()->gainDb;
        step("MIDI learn (CC 102 -> master volume)", m && m->cc == 102 && !midiLearning() && std::fabs(g - 6.0f) < 1e-3f,
             std::format("gain {:.2f} dB", g));
        run("SetChannelGain", {{"master", true}, {"gainDb", 0.0}});
        run("ClearMidiMappings", json::object());
    }
    {
        const std::string report = createDiagnosticsReport();
        step("diagnostics report", !report.empty() && fs::exists(report), report);
    }
    // undo / redo
    const std::string before = projectToJson(*project_).dump();
    step("undo", undo() && projectToJson(*project_).dump() != before);
    step("redo", redo() && projectToJson(*project_).dump() == before);
    // save / close / reopen
    step("save", save());
    json saved = projectToJson(*project_);
    closeProject();
    step("reopen", openProject(file));
    json again = projectToJson(*project_);
    saved.erase("modifiedAt");
    again.erase("modifiedAt");
    step("reopened project identical", saved == again);
    step("no plugin crashes", crashes_.empty());
    const std::string summary = std::format("SELFTEST {} ({} failed)\n", failed ? "FAILED" : "PASSED", failed);
    std::fputs(summary.c_str(), stdout);
    files::atomicWrite(folder / "selftest_report.txt", report + summary);
    return failed == 0;
}

} // namespace roy::gui
