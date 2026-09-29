#include "MusicSession.h"
#include "App.h"

#include "core/Files.h"
#include "io/AudioFile.h"
#include "plugins/Sandbox.h"
#include "support/TestSignals.h"

#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <thread>

namespace roy::gui {

namespace fs = std::filesystem;

namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string busId(App& app, const std::string& name) {
    for (auto& c : app.project().channels)
        if (c.name == name) return c.id;
    return {};
}
// Runs a command; returns "" or the command's error text.
std::string cmd(App& app, const std::string& id, const json& args) { return app.run(id, args) ? std::string() : id + ": " + app.lastError(); }
std::string needProject(App& app) { return app.hasProject() ? "" : "no project open - run step A1 first"; }

std::string setRow(App& app, SessionState& st, const char* voice, const char* text) {
    if (auto e = needProject(app); !e.empty()) return e;
    if (st.pattern.empty() || !app.project().findPattern(st.pattern)) return "no drum pattern yet - run step A3 first";
    return cmd(app, "SetRowPattern", {{"patternId", st.pattern}, {"voice", voice}, {"text", text}});
}

const plugins::PluginRecord* testPlugin(App& app, const std::string& format) {
    for (auto* r : app.pluginDb().available())
        if (r->format == format && (r->name == "RoY VST3 Gain" || r->name == "RoY Test Gain")) return r;
    return nullptr;
}

std::string loadTestPlugin(App& app, const std::string& format, std::string& slot) {
    const auto* rec = testPlugin(app, format);
    if (!rec) return "RoY TEST " + format + " plugin not found - run step F1 (scan) and wait until it finishes";
    const std::string ch = !busId(app, "DRUMS").empty() ? busId(app, "DRUMS") : app.project().master()->id;
    if (auto e = cmd(app, "AddInsert", {{"channelId", ch}, {"typeId", rec->typeId}, {"name", rec->name}}); !e.empty()) return e;
    slot = app.lastResult().value("id", "");
    return {};
}

float paramOf(App& app, const std::string& slot) {
    auto p = slot.empty() ? nullptr : app.runtime().processorForSlot(slot);
    return p && p->numParams() > 0 ? p->getParam(0) : -1.0f;
}

std::vector<SessionStep> buildSteps() {
    std::vector<SessionStep> v;
    const std::string A = "A  BEAT", B = "B  VOCAL", C = "C  VOCAL EDIT", D = "D  MIX", E = "E  EXPORT", F = "F  PLUGINS (RoY TEST)";
    // ------------------------------------------------------------------ A BEAT
    v.push_back({A, "New project", "Creates 'First Session <date>' in your projects folder.", [](App& app, SessionState& st, std::string& info) {
                     st = SessionState{};
                     const std::string name = "First Session " + files::nowCompact();
                     if (!app.newProject(files::defaultProjectsDirectory(), name, 140.0)) return std::string("could not create the project (see status bar)");
                     info = app.session().file().string();
                     return std::string();
                 }});
    v.push_back({A, "Tempo 140 BPM", "Transport bar shows 140.0 BPM.", [](App& app, SessionState&, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     return cmd(app, "SetTempo", {{"bpm", 140.0}});
                 }});
    v.push_back({A, "Create a drum pattern", "BEATS area: a Drums track and the pattern 'Beat 1'.", [](App& app, SessionState& st, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (auto e = cmd(app, "AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"role", "drums"}, {"output", busId(app, "DRUMS")}}); !e.empty()) return e;
                     st.drums = app.lastResult().value("id", "");
                     if (auto e = cmd(app, "AddPattern", {{"name", "Beat 1"}, {"steps", 16}}); !e.empty()) return e;
                     st.pattern = app.lastResult().value("id", "");
                     app.selPattern = st.pattern;
                     app.area = Area::Beats;
                     return std::string();
                 }});
    v.push_back({A, "Kick", "Kick row: steps 1, 7, 11.", [](App& app, SessionState& st, std::string&) { return setRow(app, st, "kick", "X.....x...x....."); }});
    v.push_back({A, "Snare", "Snare row: steps 5 and 13 (backbeat).", [](App& app, SessionState& st, std::string&) { return setRow(app, st, "snare", "....X.......X..."); }});
    v.push_back({A, "Hi-hat", "Closed hi-hat on every 1/16 with accents.", [](App& app, SessionState& st, std::string&) {
                     return setRow(app, st, "closed_hat", "XxxxXxxxXxxxXxXx");
                 }});
    v.push_back({A, "Load the 808", "A MIDI track '808' with the RoY 808 instrument.", [](App& app, SessionState& st, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (auto e = cmd(app, "AddTrack", {{"type", "midi"}, {"name", "808"}, {"instrument", "roy.808"}, {"role", "808"}, {"output", busId(app, "DRUMS")}});
                         !e.empty())
                         return e;
                     st.bass = app.lastResult().value("id", "");
                     return std::string();
                 }});
    v.push_back({A, "808 notes", "PIANO ROLL: an 808 line in A minor (with one slide).", [](App& app, SessionState& st, std::string&) {
                     if (st.bass.empty() || !app.project().findTrack(st.bass)) return std::string("no 808 track yet - run the previous step");
                     if (auto e = cmd(app, "AddMidiClip", {{"trackId", st.bass}, {"startBeat", 0.0}, {"lengthBeats", 16.0}, {"name", "808 Line"}}); !e.empty()) return e;
                     st.bassClip = app.lastResult().value("id", "");
                     const double notes[][3] = {{33, 0.0, 1.5}, {33, 2.5, 1.0}, {36, 4.0, 1.5}, {31, 8.0, 2.0}, {29, 12.0, 3.0}};
                     for (auto& n : notes)
                         if (auto e = cmd(app, "AddNote", {{"clipId", st.bassClip}, {"pitch", n[0]}, {"startBeat", n[1]}, {"lengthBeats", n[2]}, {"slide", n[1] == 4.0}});
                             !e.empty())
                             return e;
                     app.selMidiClip = st.bassClip;
                     return std::string();
                 }});
    v.push_back({A, "Play the pattern", "Loop 1-5 plays: kick, snare, hats and 808 together.", [](App& app, SessionState& st, std::string&) {
                     if (st.drums.empty() || st.pattern.empty()) return std::string("no drum pattern yet - run steps A3-A6");
                     if (st.patternClip.empty()) {
                         if (auto e = cmd(app, "AddPatternClip", {{"trackId", st.drums}, {"patternId", st.pattern}, {"startBeat", 0.0}, {"lengthBeats", 16.0}}); !e.empty())
                             return e;
                         st.patternClip = app.lastResult().value("id", "");
                     }
                     if (auto e = cmd(app, "SetLoop", {{"enabled", true}, {"startBeat", 0.0}, {"endBeat", 16.0}}); !e.empty()) return e;
                     app.seekBeat(0);
                     if (!app.engine().transport().isPlaying()) app.togglePlay();
                     return std::string();
                 }});
    v.push_back({A, "Arrangement in the playlist", "PLAYLIST: beat + 808 now run 8 bars (clips copied to bar 5).", [](App& app, SessionState& st, std::string&) {
                     if (st.patternClip.empty() || st.bassClip.empty()) return std::string("run steps A8 and A9 first");
                     if (auto e = cmd(app, "DuplicateClip", {{"clipId", st.patternClip}, {"startBeat", 16.0}}); !e.empty()) return e;
                     if (auto e = cmd(app, "DuplicateClip", {{"clipId", st.bassClip}, {"startBeat", 16.0}}); !e.empty()) return e;
                     if (auto e = cmd(app, "SetLoop", {{"enabled", true}, {"startBeat", 0.0}, {"endBeat", 32.0}}); !e.empty()) return e;
                     app.area = Area::Playlist;
                     return std::string();
                 }});
    v.push_back({A, "Open the mixer", "MIXER: Drums / 808 channels move while the beat plays.", [](App& app, SessionState&, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     app.area = Area::Mixer;
                     return std::string();
                 }});
    v.push_back({A, "Listen to the beat", "You hear the beat on your speakers/headphones. If not: Audio menu > Output.", [](App& app, SessionState&, std::string& info) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (!app.audioRunning()) return std::string("no audio device running - Audio menu > Output / Setup check");
                     app.seekBeat(0);
                     if (!app.engine().transport().isPlaying()) app.togglePlay();
                     info = app.audioStatus();
                     return std::string();
                 }});
    // ------------------------------------------------------------------ B VOCAL
    v.push_back({B, "Add a vocal track", "PLAYLIST: new audio track 'Vocal' routed to the VOCALS bus.", [](App& app, SessionState& st, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (app.engine().transport().isPlaying()) app.stop();
                     if (auto e = cmd(app, "AddTrack", {{"type", "audio"}, {"name", "Vocal"}, {"role", "vocal"}, {"output", busId(app, "VOCALS")}}); !e.empty()) return e;
                     st.vocal = app.lastResult().value("id", "");
                     st.vocalCh = app.lastResult().value("channelId", "");
                     app.selTrack = st.vocal;
                     app.selChannel = st.vocalCh;
                     app.area = Area::Playlist;
                     return std::string();
                 }});
    v.push_back({B, "Choose the input device", "Pick your microphone / interface in the list above this step (or Audio menu > Input).",
                 [](App& app, SessionState&, std::string& info) {
                     if (!app.audioRunning()) return std::string("no audio device running");
                     if (app.device().actualInputChannels() == 0)
                         return std::string("no input open - choose an input (Windows: Settings > Privacy > Microphone > allow desktop apps)");
                     info = std::format("{} input channel(s)", app.device().actualInputChannels());
                     return std::string();
                 }});
    v.push_back({B, "Arm the track", "The red R button of 'Vocal' lights up. Use headphones to avoid feedback.", [](App& app, SessionState& st, std::string&) {
                     if (st.vocal.empty() || !app.project().findTrack(st.vocal)) return std::string("no vocal track - run step B1");
                     return cmd(app, "ArmTrack", {{"trackId", st.vocal}, {"armed", true}});
                 }});
    v.push_back({B, "Input level", "Speak or sing: the INPUT bar above must move (best peaks around -12 dBFS).", [](App&, SessionState& st, std::string& info) {
                     info = std::format("loudest input {:.0f} dBFS", 20.0f * std::log10(std::max(1e-6f, st.maxInput)));
                     return st.maxInput > 0.001f ? std::string() : std::string("no input signal seen yet - speak into the microphone while this step is open");
                 }});
    v.push_back({B, "Count-in (1 bar)", "Metronome on, 1 bar count-in before recording.", [](App& app, SessionState&, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     return cmd(app, "SetMetronome", {{"enabled", true}, {"countInBars", 1}});
                 }});
    v.push_back({B, "Start recording", "After 4 clicks the recording runs (REC is red). Sing or rap now!", [](App& app, SessionState& st, std::string&) {
                     if (st.vocal.empty()) return std::string("no vocal track - run step B1");
                     if (app.recording()) return std::string("already recording");
                     if (app.engine().transport().isPlaying()) app.stop();
                     cmd(app, "SetLoop", {{"enabled", false}});
                     app.seekBeat(0);
                     app.toggleRecord();
                     if (!app.recording()) return std::string("recording did not start - see the status bar (track armed? input open?)");
                     st.recordStartSec = now();
                     return std::string();
                 }});
    v.push_back({B, "Record 10-20 seconds", "Keep going until the timer shows at least 10 s.", [](App& app, SessionState& st, std::string& info) {
                     if (!app.recording() || st.recordStartSec < 0) return std::string("not recording - run the previous step");
                     const double t = now() - st.recordStartSec;
                     info = std::format("{:.0f} s recorded", t);
                     return t >= 10.0 ? std::string() : std::format("only {:.0f} s so far - record at least 10 s", t);
                 }});
    v.push_back({B, "Stop", "The new take appears on the Vocal track.", [](App& app, SessionState&, std::string&) {
                     if (!app.recording()) return std::string("not recording");
                     app.stop();
                     return std::string();
                 }});
    v.push_back({B, "Play the recording", "Your voice plays back together with the beat.", [](App& app, SessionState& st, std::string& info) {
                     const Track* t = st.vocal.empty() ? nullptr : app.project().findTrack(st.vocal);
                     if (!t) return std::string("no vocal track");
                     if (t->takes.empty() && t->audioClips.empty()) return std::string("no take yet - wait a second after stopping, then press again");
                     if (!t->comp.empty()) { // the take becomes a normal clip for editing (original file untouched)
                         if (auto e = cmd(app, "FlattenComp", {{"trackId", st.vocal}}); !e.empty()) return e;
                     }
                     t = app.project().findTrack(st.vocal);
                     if (t->audioClips.empty()) return std::string("no clip on the vocal track");
                     st.vocalClip = t->audioClips.back().id;
                     app.selClip = st.vocalClip;
                     app.seekBeat(0);
                     if (!app.engine().transport().isPlaying()) app.togglePlay();
                     info = std::format("take {:.1f} beats long", t->audioClips.back().lengthBeats);
                     return std::string();
                 }});
    // ------------------------------------------------------------------ C VOCAL EDIT
    auto needClip = [](App& app, SessionState& st) -> std::string {
        if (st.vocalClip.empty() || !app.project().findAudioClip(st.vocalClip)) return "no vocal clip - finish part B first";
        return {};
    };
    v.push_back({C, "Pitch analysis", "Detects the notes you sang.", [needClip](App& app, SessionState& st, std::string& info) {
                     if (auto e = needClip(app, st); !e.empty()) return e;
                     if (app.engine().transport().isPlaying()) app.stop();
                     if (auto e = cmd(app, "PitchAnalysis", {{"clipId", st.vocalClip}}); !e.empty()) return e;
                     info = std::format("{} notes", app.lastResult().value("notes", json::array()).size());
                     return std::string();
                 }});
    v.push_back({C, "Pitch editor", "VOCALS area: waveform, pitch curve, detected and target notes.", [needClip](App& app, SessionState& st, std::string&) {
                     if (auto e = needClip(app, st); !e.empty()) return e;
                     app.selClip = st.vocalClip;
                     app.area = Area::Vocals;
                     return std::string();
                 }});
    v.push_back({C, "OFF-KEY display", "Notes outside the key are marked OFF KEY, unsure ones UNCERTAIN.", [needClip](App& app, SessionState& st, std::string& info) {
                     if (auto e = needClip(app, st); !e.empty()) return e;
                     if (auto e = cmd(app, "PitchEditorData", {{"clipId", st.vocalClip}}); !e.empty()) return e;
                     const json counts = app.lastResult().value("counts", json::object()); // keep alive while iterating
                     for (auto& [k, c] : counts.items()) info += std::format("{} {}  ", k, c.get<int>());
                     return std::string();
                 }});
    v.push_back({C, "Pitch Guardian preview", "Tuned version plays; the ORIGINAL recording file is never changed.", [needClip](App& app, SessionState& st, std::string& info) {
                     if (auto e = needClip(app, st); !e.empty()) return e;
                     if (auto e = cmd(app, "PitchGuardian", {{"clipId", st.vocalClip}, {"mode", "lock"}, {"offKeyFilter", true}}); !e.empty()) return e;
                     const size_t checked = app.lastResult().value("notes", json::array()).size();
                     const int corrected = app.lastResult().value("corrected", 0);
                     const AudioClip* c = app.project().findAudioClip(st.vocalClip);
                     if (checked == 0) return std::string("no sung notes found in the recording (silence? input level?) - record again with voice");
                     info = std::format("{} notes checked, {} corrected", checked, corrected);
                     if (corrected == 0) {
                         info += " - you sang in tune, nothing to correct (sing a note a bit off and repeat to hear a correction)";
                         return std::string();
                     }
                     if (!c || c->rawAssetId.empty()) return std::string("corrected but the original is not referenced - please report this");
                     app.seekBeat(0);
                     if (!app.engine().transport().isPlaying()) app.togglePlay();
                     return std::string();
                 }});
    v.push_back({C, "A/B original / corrected", "Switch with the A / B buttons in VOCALS - both play in sync.", [needClip](App& app, SessionState& st, std::string& info) {
                     if (auto e = needClip(app, st); !e.empty()) return e;
                     const AudioClip* c = app.project().findAudioClip(st.vocalClip);
                     if (c->tunedAssetId.empty()) {
                         info = "not applicable: nothing was corrected in C4";
                         return std::string();
                     }
                     if (auto e = cmd(app, "VocalAB", {{"clipId", st.vocalClip}, {"use", "original"}}); !e.empty()) return e;
                     if (auto e = cmd(app, "VocalAB", {{"clipId", st.vocalClip}, {"use", "corrected"}}); !e.empty()) return e;
                     info = "now playing: corrected";
                     return std::string();
                 }});
    // ------------------------------------------------------------------ D MIX
    v.push_back({D, "Vocal and beat level", "MIXER: Vocal -2 dB, Drums -4 dB, 808 -5 dB (all faders stay movable).", [](App& app, SessionState& st, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     const std::pair<std::string, double> levels[] = {{st.vocal, -2.0}, {st.drums, -4.0}, {st.bass, -5.0}};
                     for (auto& [track, db] : levels)
                         if (!track.empty() && app.project().findTrack(track))
                             if (auto e = cmd(app, "SetChannelGain", {{"trackId", track}, {"gainDb", db}}); !e.empty()) return e;
                     app.area = Area::Mixer;
                     return std::string();
                 }});
    auto vocalInsert = [](const char* type, const char* name) {
        return [type, name](App& app, SessionState& st, std::string&) -> std::string {
            if (st.vocalCh.empty() || !app.project().findChannel(st.vocalCh)) return "no vocal track - run part B";
            if (auto e = cmd(app, "AddInsert", {{"channelId", st.vocalCh}, {"typeId", type}, {"name", name}}); !e.empty()) return e;
            app.selChannel = st.vocalCh;
            app.selSlot = app.lastResult().value("id", "");
            return std::string();
        };
    };
    v.push_back({D, "EQ", "RoY EQ on the Vocal channel - open it in MIXER to see/change every band.", vocalInsert("roy.eq", "RoY EQ")});
    v.push_back({D, "Compression", "RoY Compressor on the Vocal channel.", vocalInsert("roy.compressor", "RoY Comp")});
    v.push_back({D, "De-esser", "RoY De-Esser on the Vocal channel.", vocalInsert("roy.deesser", "RoY De-Esser")});
    v.push_back({D, "Reverb (send)", "A bus 'FX Reverb' with RoY Reverb; the vocal sends -12 dB to it.", [](App& app, SessionState& st, std::string&) {
                     if (st.vocalCh.empty() || !app.project().findChannel(st.vocalCh)) return std::string("no vocal track - run part B");
                     if (st.fxBus.empty() || !app.project().findChannel(st.fxBus)) {
                         if (auto e = cmd(app, "AddBus", {{"name", "FX Reverb"}}); !e.empty()) return e;
                         st.fxBus = app.lastResult().value("id", "");
                         if (auto e = cmd(app, "AddInsert", {{"channelId", st.fxBus}, {"typeId", "roy.reverb"}, {"name", "RoY Reverb"}}); !e.empty()) return e;
                     }
                     return cmd(app, "AddSend", {{"channelId", st.vocalCh}, {"target", st.fxBus}, {"levelDb", -12.0}});
                 }});
    // ------------------------------------------------------------------ E EXPORT
    v.push_back({E, "Choose the export folder", "Use 'Browse...' above (default: the project's Exports folder).", [](App& app, SessionState&, std::string& info) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     std::error_code ec;
                     fs::create_directories(app.currentExportFolder(), ec);
                     info = app.currentExportFolder().string();
                     return fs::is_directory(app.currentExportFolder(), ec) ? std::string() : std::string("folder cannot be created");
                 }});
    auto exportAs = [](const char* fmt) {
        return [fmt](App& app, SessionState&, std::string& info) -> std::string {
            if (auto e = needProject(app); !e.empty()) return e;
            if (app.engine().transport().isPlaying()) app.stop();
            json a = {{"format", fmt}, {"folder", app.currentExportFolder().string()}, {"name", app.project().name}};
            if (std::string(fmt) == "mp3") a["bitrate"] = 320;
            if (auto e = cmd(app, "Export", a); !e.empty()) return e;
            for (auto& f : app.lastResult().value("files", json::array()))
                info = std::format("{} ({:.1f} LUFS)", fs::path(f.value("path", "")).filename().string(), f.value("lufs", 0.0));
            return std::string();
        };
    };
    v.push_back({E, "Export WAV", "24-bit WAV of the whole song.", exportAs("wav")});
    v.push_back({E, "Export FLAC", "Lossless FLAC.", exportAs("flac")});
    v.push_back({E, "Export MP3", "MP3 320 kbit/s (LAME).", exportAs("mp3")});
    v.push_back({E, "Open the export folder", "Windows Explorer opens the folder with the three files.", [](App& app, SessionState&, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     app.openFolder(app.currentExportFolder());
                     return std::string();
                 }});
    // ------------------------------------------------------------------ F PLUGINS
    v.push_back({F, "Scan the RoY TEST plugins", "Scans the Plugins folder next to RoY (runs in the background, a few seconds).",
                 [](App& app, SessionState&, std::string& info) {
                     if (!testPlugin(app, "vst3") || !testPlugin(app, "clap")) {
                         if (!app.pluginScanRunning()) app.startPluginScan(false, false);
                         return std::string("scan started - press DO IT again when it is finished");
                     }
                     info = "RoY VST3 Gain + RoY Test Gain (CLAP) found";
                     return std::string();
                 }});
    v.push_back({F, "Load the VST3 + CLAP test plugin", "Both appear on the DRUMS bus in MIXER (each runs in its own sandbox process).",
                 [](App& app, SessionState& st, std::string&) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (auto e = loadTestPlugin(app, "vst3", st.vst3Slot); !e.empty()) return e;
                     return loadTestPlugin(app, "clap", st.clapSlot);
                 }});
    v.push_back({F, "Open the plugin editors", "Two plugin windows open (close them when you have seen them).", [](App& app, SessionState& st, std::string&) {
                     if (st.vst3Slot.empty() || st.clapSlot.empty()) return std::string("load the plugins first (F2)");
                     if (auto e = cmd(app, "OpenPluginEditor", {{"slotId", st.vst3Slot}}); !e.empty()) return e;
                     return cmd(app, "OpenPluginEditor", {{"slotId", st.clapSlot}});
                 }});
    v.push_back({F, "Change a parameter", "Gain of both plugins is set to a new value (visible in the plugin parameter list).", [](App& app, SessionState& st, std::string&) {
                     for (auto* slot : {&st.vst3Slot, &st.clapSlot}) {
                         auto p = slot->empty() ? nullptr : app.runtime().processorForSlot(*slot);
                         if (!p || p->numParams() == 0) return std::string("plugin not loaded - run F2");
                         const auto& pi = p->paramInfo(0);
                         const float v = pi.minValue + (pi.maxValue - pi.minValue) * 0.37f;
                         if (auto e = cmd(app, "SetParam", {{"slotId", *slot}, {"paramId", pi.id}, {"value", v}}); !e.empty()) return e;
                         (slot == &st.vst3Slot ? st.vst3Expected : st.clapExpected) = v;
                     }
                     return std::string();
                 }});
    v.push_back({F, "Automation", "An automation lane for the VST3 gain (PLAYLIST automation).", [](App& app, SessionState& st, std::string&) {
                     auto p = st.vst3Slot.empty() ? nullptr : app.runtime().processorForSlot(st.vst3Slot);
                     MixerChannel* owner = nullptr;
                     if (!p || !app.project().findSlot(st.vst3Slot, &owner) || !owner) return std::string("VST3 plugin not loaded - run F2");
                     return cmd(app, "CreateAutomation", {{"channelId", owner->id}, {"slotId", st.vst3Slot}, {"paramId", p->paramInfo(0).id},
                                                          {"points", {{0.0, st.vst3Expected}, {32.0, st.vst3Expected}}}});
                 }});
    v.push_back({F, "Save, close and reopen", "The project is saved, closed and opened again (plugins restart).", [](App& app, SessionState&, std::string& info) {
                     if (auto e = needProject(app); !e.empty()) return e;
                     if (app.engine().transport().isPlaying()) app.stop();
                     if (!app.save()) return std::string("save failed (see status bar)");
                     const fs::path file = app.session().file();
                     if (!app.openProject(file)) return std::string("reopen failed (see status bar)");
                     info = file.filename().string();
                     return std::string();
                 }});
    v.push_back({F, "Plugin state restored", "Both plugins have their changed gain again after reopening.", [](App& app, SessionState& st, std::string& info) {
                     const float a = paramOf(app, st.vst3Slot), b = paramOf(app, st.clapSlot);
                     info = std::format("VST3 {:.3f} (expected {:.3f}), CLAP {:.3f} (expected {:.3f})", a, st.vst3Expected, b, st.clapExpected);
                     if (st.vst3Expected < 0 || st.clapExpected < 0) return std::string("run F4 first");
                     return std::fabs(a - st.vst3Expected) < 1e-3f && std::fabs(b - st.clapExpected) < 1e-3f ? std::string() : std::string("state NOT restored");
                 }});
    return v;
}
} // namespace

const std::vector<SessionStep>& musicSessionSteps() {
    static const std::vector<SessionStep> steps = buildSteps();
    return steps;
}

std::string sessionResultsFile() { return (files::userDataDirectory() / "FirstRealMusicSession_results.md").string(); }

bool runSessionStep(App& app, SessionState& st, size_t i) {
    const auto& steps = musicSessionSteps();
    if (i >= steps.size()) return false;
    std::string info;
    std::string err;
    try {
        err = steps[i].run(app, st, info);
    } catch (const std::exception& e) {
        err = std::string("internal error: ") + e.what();
    }
    const std::string result = err.empty() ? "PASS" + (info.empty() ? "" : " - " + info) : "FAIL - " + err;
    st.results[i] = result;
    const std::string line = std::format("| {} | {} | {} | {} |\n", files::nowIso8601(), steps[i].group, steps[i].title, result);
    std::error_code ec;
    const bool fresh = !fs::exists(sessionResultsFile(), ec);
    std::ofstream f(sessionResultsFile(), std::ios::app);
    if (fresh) f << "# FIRST REAL MUSIC SESSION - results\n\n| Time (UTC) | Part | Step | Result |\n|---|---|---|---|\n";
    f << line;
    app.message(err.empty() ? 0 : 2, steps[i].title + ": " + result);
    return err.empty();
}

bool runSessionSelfTest(App& app, const std::string& folder) {
    auto pump = [&](double seconds) {
        const double end = now() + seconds;
        while (now() < end) {
            app.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    SessionState st;
    const auto& steps = musicSessionSteps();
    std::string report = "# FIRST REAL MUSIC SESSION - automated run\n\n| # | Part | Step | Result |\n|---|---|---|---|\n";
    int failed = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        std::string result;
        if (s.title == "Input level" || s.title == "Open the export folder") {
            result = "SKIPPED - needs a person / real microphone";
        } else {
            if (s.title == "Record 10-20 seconds") pump(10.5);
            bool ok = runSessionStep(app, st, i);
            if (!ok && s.title == "Scan the RoY TEST plugins") {
                for (int k = 0; k < 3000 && app.pluginScanRunning(); ++k) pump(0.02);
                ok = runSessionStep(app, st, i);
            }
            if (s.title == "Stop") pump(1.0); // the take is added on the next ticks
            if (s.title == "Play the recording" && ok && !st.vocalClip.empty()) {
                // automated runs record from a silent (null) input: continue with the synthetic
                // test vocal so the pitch steps have something to work on
                auto data = app.runtime().asset(app.project(), app.project().findAudioClip(st.vocalClip)->assetId);
                float peak = 0;
                if (data)
                    for (auto& ch : data->channels)
                        for (float x : ch) peak = std::max(peak, std::fabs(x));
                if (peak < 0.001f) {
                    const fs::path f = files::userDataDirectory() / "session_test_synthetic_vocal.wav";
                    std::error_code ec;
                    fs::remove(f, ec);
                    writeWavFile(f, {support::syntheticVocal(48000.0, 8.0)}, 48000.0, SampleFormat::Pcm24, false, nullptr);
                    if (app.run("ImportAudio", {{"path", f.string()}, {"trackId", st.vocal}, {"startBeat", 0.0}})) {
                        const Track* t = app.project().findTrack(st.vocal);
                        st.vocalClip = t->audioClips.back().id;
                        st.results[i] += " (silent null input: continuing with the synthetic test vocal)";
                    }
                }
            }
            if (s.title == "Listen to the beat" || s.title == "Play the pattern") pump(1.0);
            if (s.title == "Choose the input device" && !ok) result = "SKIPPED - " + st.results[i]; // null device without input
            else result = st.results[i];
            if (result.rfind("FAIL", 0) == 0) ++failed;
        }
        report += std::format("| {} | {} | {} | {} |\n", i + 1, s.group, s.title, result);
        std::printf("%s %s\n", result.substr(0, 4).c_str(), s.title.c_str());
    }
    report += std::format("\n**{}** ({} failed)\n", failed ? "SESSION TEST FAILED" : "SESSION TEST PASSED", failed);
    std::error_code ec;
    fs::create_directories(folder, ec);
    files::atomicWrite(fs::path(folder) / "session_test_report.md", report);
    return failed == 0;
}

} // namespace roy::gui
