#include "Ui.h"

#include "core/Files.h"
#include "midi/MidiLearn.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace roy::gui {

namespace {
char g_newName[128] = "New Song";
char g_newFolder[512] = "";
char g_openPath[1024] = "";
float g_newBpm = 140.0f;
bool g_openNew = false, g_openOpen = false;

// Output / input device submenus (empty name = system default); a change is remembered.
void audioDeviceMenus(App& app) {
    auto cfg = app.audioConfig();
    if (ImGui::BeginMenu(std::format("Output: {}", cfg.outputDevice.empty() ? "system default" : cfg.outputDevice).c_str())) {
        if (ImGui::MenuItem("System default", nullptr, cfg.outputDevice.empty())) {
            cfg.outputDevice.clear();
            app.changeAudio(cfg);
        }
        for (auto& d : app.device().outputDevices())
            if (ImGui::MenuItem((d.name + (d.isDefault ? "  (default)" : "")).c_str(), nullptr, cfg.outputDevice == d.name)) {
                cfg.outputDevice = d.name;
                app.changeAudio(cfg);
            }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(std::format("Input: {}", !cfg.enableInput ? "off" : cfg.inputDevice.empty() ? "system default" : cfg.inputDevice).c_str())) {
        if (ImGui::MenuItem("Off (playback only)", nullptr, !cfg.enableInput)) {
            cfg.enableInput = false;
            app.changeAudio(cfg);
        }
        if (ImGui::MenuItem("System default", nullptr, cfg.enableInput && cfg.inputDevice.empty())) {
            cfg.enableInput = true;
            cfg.inputDevice.clear();
            app.changeAudio(cfg);
        }
        for (auto& d : app.device().inputDevices())
            if (ImGui::MenuItem((d.name + (d.isDefault ? "  (default)" : "")).c_str(), nullptr, cfg.enableInput && cfg.inputDevice == d.name)) {
                cfg.enableInput = true;
                cfg.inputDevice = d.name;
                app.changeAudio(cfg);
            }
        ImGui::EndMenu();
    }
    ImGui::Separator();
}

// SETUP CHECK: shown on the first start (and from the Audio / Help menu). Everything here can be
// changed later; "Finish" remembers that the check was done.
void setupCheck(App& app) {
    if (!app.showFirstRun) return;
    if (!ImGui::IsPopupOpen("Setup check")) ImGui::OpenPopup("Setup check");
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(620 * dpi, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Setup check", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "Welcome to RoY Studio - let's check your setup (1 minute)");
    ImGui::TextDisabled("Everything can be changed later in the Audio menu.");
    ImGui::Separator();

    sectionTitle("1  AUDIO OUTPUT");
    ImGui::TextWrapped("%s", app.audioStatus().c_str());
    auto cfg = app.audioConfig();
    ImGui::SetNextItemWidth(360 * dpi);
    if (ImGui::BeginCombo("Output device", cfg.outputDevice.empty() ? "System default" : cfg.outputDevice.c_str())) {
        if (ImGui::Selectable("System default", cfg.outputDevice.empty())) {
            cfg.outputDevice.clear();
            app.changeAudio(cfg);
        }
        for (auto& d : app.device().outputDevices())
            if (ImGui::Selectable(d.name.c_str(), cfg.outputDevice == d.name)) {
                cfg.outputDevice = d.name;
                app.changeAudio(cfg);
            }
        ImGui::EndCombo();
    }
    int bi = 2;
    const int buffers[] = {64, 128, 256, 512, 1024};
    for (int i = 0; i < 5; ++i)
        if (buffers[i] == cfg.bufferSize) bi = i;
    ImGui::SetNextItemWidth(120 * dpi);
    if (ImGui::Combo("Buffer", &bi, "64\0" "128\0" "256\0" "512\0" "1024\0")) {
        cfg.bufferSize = buffers[bi];
        app.changeAudio(cfg);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("smaller = less latency for recording, larger = safer with many plugins");
    if (goldButton("Play test tone")) app.playTestTone();
    ImGui::SameLine();
    ImGui::TextDisabled("you should hear a 1-second beep (440 Hz)");

    sectionTitle("2  AUDIO INPUT (microphone / interface)");
    ImGui::SetNextItemWidth(360 * dpi);
    if (ImGui::BeginCombo("Input device", !cfg.enableInput ? "Off" : cfg.inputDevice.empty() ? "System default" : cfg.inputDevice.c_str())) {
        if (ImGui::Selectable("Off (playback only)", !cfg.enableInput)) {
            cfg.enableInput = false;
            app.changeAudio(cfg);
        }
        if (ImGui::Selectable("System default", cfg.enableInput && cfg.inputDevice.empty())) {
            cfg.enableInput = true;
            cfg.inputDevice.clear();
            app.changeAudio(cfg);
        }
        for (auto& d : app.device().inputDevices())
            if (ImGui::Selectable(d.name.c_str(), cfg.enableInput && cfg.inputDevice == d.name)) {
                cfg.enableInput = true;
                cfg.inputDevice = d.name;
                app.changeAudio(cfg);
            }
        ImGui::EndCombo();
    }
    static float shownL = 0, shownR = 0; // meter with a short fall-off
    shownL = std::max(app.engine().inputPeak(0), shownL * 0.9f);
    shownR = std::max(app.engine().inputPeak(1), shownR * 0.9f);
    { // horizontal meter, -60..0 dBFS, one bar per input channel
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = 360 * dpi, h = 7 * dpi;
        for (int c = 0; c < 2; ++c) {
            const float v = c == 0 ? shownL : shownR;
            const float x = std::clamp((20.0f * std::log10(std::max(1e-6f, v)) + 60.0f) / 60.0f, 0.0f, 1.0f);
            const ImVec2 a(p0.x, p0.y + c * (h + 2)), b(p0.x + w, a.y + h);
            dl->AddRectFilled(a, b, col::rgb(0x2A2A30));
            dl->AddRectFilled(a, ImVec2(a.x + w * x, b.y), x > 59.0f / 60.0f ? col::Red : x > 48.0f / 60.0f ? col::Orange : col::Green);
        }
        for (int dbMark : {-48, -24, -12, -6}) {
            const float x = p0.x + w * (dbMark + 60) / 60.0f;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + 2 * h + 2), col::rgb(0x000000, 160));
        }
        ImGui::Dummy(ImVec2(w, 2 * h + 2));
    }
    ImGui::SameLine();
    const float db = 20.0f * std::log10(std::max(1e-6f, std::max(shownL, shownR)));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(db > -1.0f ? col::Red : db > -60.0f ? col::Green : col::IvoryDim), "%s",
                       db > -1.0f ? "TOO LOUD - lower the input gain" : db > -60.0f ? std::format("{:.0f} dBFS", db).c_str() : "no signal");
    ImGui::TextDisabled("Speak or play into your input: the bar must move. Aim for peaks around -12 dBFS.");

    sectionTitle("3  MIDI KEYBOARD / CONTROLLER");
    if (auto* mi = app.midiInput()) {
        const auto devs = mi->devices();
        if (devs.empty()) ImGui::TextDisabled("no MIDI input connected (connect one any time - it is detected automatically)");
        for (auto& d : devs) ImGui::BulletText("%s %s", d.name.c_str(), mi->isOpen(d.id) ? "(open)" : "(off)");
        ImGui::TextDisabled("press a key: %s", mi->lastMessageText().c_str());
    }

    sectionTitle("4  PLUGINS (VST3 / CLAP)");
    ImGui::TextDisabled("Plugins are scanned in a separate process - a broken plugin cannot crash RoY.");
    if (app.pluginScanRunning()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "scanning...");
    else if (ImGui::Button("Scan plugins now")) app.startPluginScan(false, false);

    ImGui::Separator();
    if (goldButton("Finish")) {
        app.finishFirstRun();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later")) { // shown again on the next start
        app.showFirstRun = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void shortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) app.togglePlay();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? app.redo() : app.undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) app.redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) app.save();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) app.showPalette = true;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) app.toggleRecord();
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) app.seekBeat(0);
    if (app.midiLearning() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) app.cancelMidiLearn();
    const ImGuiKey fkeys[] = {ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9};
    for (int i = 0; i < static_cast<int>(Area::Count); ++i)
        if (ImGui::IsKeyPressed(fkeys[i], false)) app.area = static_cast<Area>(i);
}

void menuBar(App& app) {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Gold));
    ImGui::TextUnformatted("RoY STUDIO");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("ULTIMATE");
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project...")) g_openNew = true;
        if (ImGui::MenuItem("Open Project...")) g_openOpen = true;
        if (ImGui::MenuItem("Save", "Ctrl+S", false, app.hasProject())) app.save();
        ImGui::Separator();
        if (ImGui::MenuItem("Export...", nullptr, false, app.hasProject())) app.area = Area::Master;
        if (ImGui::MenuItem("Close Project", nullptr, false, app.hasProject())) app.closeProject();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool can = app.hasProject();
        if (ImGui::MenuItem(can && app.undoManager().canUndo() ? ("Undo " + app.undoManager().undoName()).c_str() : "Undo", "Ctrl+Z", false,
                            can && app.undoManager().canUndo()))
            app.undo();
        if (ImGui::MenuItem(can && app.undoManager().canRedo() ? ("Redo " + app.undoManager().redoName()).c_str() : "Redo", "Ctrl+Y", false,
                            can && app.undoManager().canRedo()))
            app.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette", "Ctrl+P")) app.showPalette = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int i = 0; i < static_cast<int>(Area::Count); ++i)
            if (ImGui::MenuItem(areaName(static_cast<Area>(i)), std::format("F{}", i + 1).c_str(), app.area == static_cast<Area>(i)))
                app.area = static_cast<Area>(i);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Audio")) {
        ImGui::TextDisabled("%s", app.audioStatus().c_str());
        auto cfg = app.audioConfig();
        audioDeviceMenus(app);
        for (int b : {64, 128, 256, 512, 1024})
            if (ImGui::MenuItem(std::format("Buffer {} samples", b).c_str(), nullptr, cfg.bufferSize == b)) {
                cfg.bufferSize = b;
                app.changeAudio(cfg);
            }
        ImGui::Separator();
        for (double sr : {44100.0, 48000.0, 96000.0})
            if (ImGui::MenuItem(std::format("{:.0f} Hz", sr).c_str(), nullptr, cfg.sampleRate == sr)) {
                cfg.sampleRate = sr;
                app.changeAudio(cfg);
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Setup check (audio / MIDI / plugins)...")) app.showFirstRun = true;
        ImGui::Separator();
        ImGui::TextDisabled("MIDI INPUTS");
        if (auto* mi = app.midiInput()) {
            const auto devs = mi->devices();
            if (devs.empty()) ImGui::TextDisabled("no MIDI input found - connect a keyboard (detected automatically)");
            for (auto& d : devs) {
                const bool open = mi->isOpen(d.id);
                if (ImGui::MenuItem(d.name.c_str(), nullptr, open)) app.setMidiInputEnabled(d.id, !open);
            }
            ImGui::TextDisabled("last: %s", mi->lastMessageText().c_str());
            const std::string target = app.liveMidiTrackName();
            ImGui::TextDisabled("plays: %s", target.empty() ? "(select a track with an instrument)" : target.c_str());
            if (ImGui::MenuItem("MIDI panic (all notes off)")) app.midiPanic();
            if (app.hasProject() && ImGui::BeginMenu("MIDI controller mappings")) {
                auto& maps = app.project().midiMappings;
                if (maps.empty()) ImGui::TextDisabled("none - right-click a fader, knob or plugin parameter > MIDI Learn");
                std::string removeId;
                for (auto& m : maps) {
                    const bool ok = midi::mappingTargetExists(app.project(), &app.runtime(), m);
                    const std::string line = std::format("CC {:3}{}  ->  {}{}", m.cc, m.channel >= 0 ? std::format(" ch {:2}", m.channel + 1) : " any  ",
                                                         midi::targetName(app.project(), &app.runtime(), m), ok ? "" : "  (target missing)");
                    if (ImGui::MenuItem(line.c_str(), "remove")) removeId = m.id;
                }
                if (!removeId.empty()) app.run("RemoveMidiMapping", {{"mappingId", removeId}});
                if (!maps.empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Remove all mappings")) app.run("ClearMidiMappings", json::object());
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("RoY Studio %s", ROY_VERSION_STRING);
        if (ImGui::MenuItem("Setup check (audio / MIDI / plugins)...")) app.showFirstRun = true;
        if (ImGui::MenuItem("Create diagnostics report")) app.createDiagnosticsReport();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("System, audio + MIDI devices, plugin scan results, crash reports and the end of the log in one file\n"
                              "(home folder and user name replaced). Nothing is sent anywhere.");
        ImGui::TextDisabled("logs, crash reports: %s", files::userDataDirectory().string().c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void dialogs(App& app) {
    if (g_openNew) {
        ImGui::OpenPopup("New Project");
        g_openNew = false;
        if (!g_newFolder[0]) std::snprintf(g_newFolder, sizeof(g_newFolder), "%s", (files::userDataDirectory() / "Projects").string().c_str());
    }
    if (g_openOpen) {
        ImGui::OpenPopup("Open Project");
        g_openOpen = false;
    }
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", g_newName, sizeof(g_newName));
        ImGui::InputText("Folder", g_newFolder, sizeof(g_newFolder));
        ImGui::SliderFloat("BPM", &g_newBpm, 50, 220, "%.0f");
        ImGui::TextDisabled("A new folder with Audio/, Backups/, RECOVERY/ ... is created. Nothing is overwritten.");
        if (goldButton("Create")) {
            app.newProject(g_newFolder, g_newName, g_newBpm);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(".roy file", g_openPath, sizeof(g_openPath));
        const auto info = ProjectSession::inspect(g_openPath);
        if (info.crashed && info.snapshotAvailable) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "This project was not closed properly. Recovery snapshot from %s.",
                               info.snapshotTime.c_str());
            if (goldButton("RECOVER PROJECT")) {
                app.openProject(g_openPath, OpenMode::RecoverProject);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("OPEN LAST STABLE")) {
                app.openProject(g_openPath, OpenMode::OpenLastStable);
                ImGui::CloseCurrentPopup();
            }
        }
        if (info.lockOwnerAlive) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "Already open in another RoY Studio window.");
        if (ImGui::Button("Open")) {
            app.openProject(g_openPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open (Safe Mode)")) {
            app.openProject(g_openPath, OpenMode::Normal, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void statusBar(App& app) {
    const auto st = app.engine().stats();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::rgb(0x0E0E11)));
    ImGui::BeginChild("status", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    const ImU32 cpuC = st.cpuLoad > 0.8 ? col::Red : st.cpuLoad > 0.5 ? col::Orange : col::Green;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cpuC), "CPU %3.0f%%", st.cpuLoad * 100.0);
    ImGui::SameLine();
    ImGui::TextDisabled("| xruns %llu | %s", static_cast<unsigned long long>(st.overloads), app.audioStatus().c_str());
    if (app.midiLearning()) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "| MIDI LEARN: move a controller for %s (Esc cancels)", app.midiLearnLabel().c_str());
    }
    if (auto* mi = app.midiInput(); mi && mi->messageCount() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| MIDI %s -> %s", mi->lastMessageText().c_str(), app.liveMidiTrackName().c_str());
    }
    if (app.hasProject()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s", app.session().file().filename().string().c_str(), app.dirty() ? " *" : "");
    }
    if (!app.log.empty()) {
        const auto& l = app.log.back();
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(l.level >= 2 ? col::Red : l.level == 1 ? col::Orange : col::IvoryDim), "| %s", l.text.c_str());
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            for (auto& x : app.log) ImGui::TextUnformatted(x.text.c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void welcome(App& app) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + avail.x * 0.3f, ImGui::GetCursorPosY() + avail.y * 0.3f));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Gold));
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextUnformatted("RoY STUDIO ULTIMATE");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Beats - Rap - Vocals - Mix - Master");
    ImGui::Spacing();
    if (goldButton("New Project", ImVec2(200, 0))) g_openNew = true;
    if (ImGui::Button("Open Project", ImVec2(200, 0))) g_openOpen = true;
    if (ImGui::Button("Demo Project", ImVec2(200, 0))) app.buildDemo(files::userDataDirectory() / "Projects");
    ImGui::EndGroup();
}
} // namespace

void drawTransport(App& app) {
    const float h = ImGui::GetFrameHeight() * 1.6f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::Panel));
    ImGui::BeginChild("transport", ImVec2(0, h + ImGui::GetStyle().WindowPadding.y * 2), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    const bool playing = app.engine().transport().isPlaying();
    const ImVec2 bs(h * 1.3f, h);
    if (ImGui::Button("|<", bs)) app.seekBeat(0);
    ImGui::SameLine();
    if (toggleButton(playing ? "STOP" : "PLAY", playing, col::Green, ImVec2(h * 2.0f, h))) app.togglePlay();
    ImGui::SameLine();
    if (toggleButton("REC", app.recording(), col::Red, bs)) app.toggleRecord();
    ImGui::SameLine();
    if (app.hasProject()) {
        auto& p = app.project();
        const bool loop = p.loop.enabled;
        if (toggleButton("LOOP", loop, col::Orange, bs))
            app.run("SetLoop", {{"enabled", !loop}, {"startBeat", p.loop.startBeat}, {"endBeat", p.loop.endBeat > p.loop.startBeat ? p.loop.endBeat : 16.0}});
        ImGui::SameLine();
        const bool metro = p.settings.metronome;
        if (toggleButton("CLICK", metro, col::Gold, bs)) app.run("SetMetronome", {{"enabled", !metro}});
        ImGui::SameLine();
    }
    // position display
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::Obsidian));
    ImGui::BeginChild("pos", ImVec2(ImGui::CalcTextSize("0000.0.00   00:00.000").x + 24, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Gold));
    ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted(app.positionText().c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (app.hasProject()) {
        auto& p = app.project();
        ImGui::SameLine();
        float bpm = static_cast<float>(p.tempo.tempoAt(0));
        ImGui::SetNextItemWidth(90);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::DragFloat("##bpm", &bpm, 0.1f, 40, 250, "%.1f BPM");
        if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetTempo", {{"bpm", bpm}, {"atBeat", 0.0}});
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Ivory), "%s", p.key.name().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d", p.tempo.signatureAtBar(0).numerator, p.tempo.signatureAtBar(0).denominator);
        // master meter
        if (auto m = app.runtime().channelParams(p.master() ? p.master()->id : "")) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 220);
            const float l = m->peakL.exchange(0.0f), r = m->peakR.exchange(0.0f);
            ImGui::BeginGroup();
            ImGui::TextDisabled("MASTER");
            ImGui::EndGroup();
            ImGui::SameLine();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float w = 140, bh = h * 0.35f;
            for (int c = 0; c < 2; ++c) {
                const float pk = c ? r : l;
                const float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -90.0f;
                const float n = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
                const ImVec2 a(pos.x, pos.y + c * (bh + 3) + 4);
                dl->AddRectFilled(a, ImVec2(a.x + w, a.y + bh), col::Obsidian, 2);
                dl->AddRectFilled(a, ImVec2(a.x + w * n, a.y + bh), db > -1 ? col::Red : db > -6 ? col::Orange : col::Gold, 2);
            }
            ImGui::Dummy(ImVec2(w, h));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawStudio(App& app) {
    shortcuts(app);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("RoY Studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    menuBar(app);
    dialogs(app);
    setupCheck(app);
    drawTransport(app);

    const float statusH = ImGui::GetFrameHeight() + 6;
    const float browserW = 250.0f * ImGui::GetIO().FontGlobalScale + ImGui::GetFontSize() * 6;
    ImGui::BeginChild("browserPane", ImVec2(browserW, -statusH), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    drawBrowser(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mainPane", ImVec2(0, -statusH), ImGuiChildFlags_None);
    if (!app.hasProject()) {
        welcome(app);
    } else {
        if (ImGui::BeginTabBar("areas", ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int i = 0; i < static_cast<int>(Area::Count); ++i) {
                const Area a = static_cast<Area>(i);
                ImGuiTabItemFlags f = app.area == a ? ImGuiTabItemFlags_SetSelected : 0;
                bool open = ImGui::BeginTabItem(areaName(a), nullptr, f);
                if (ImGui::IsItemClicked()) app.area = a;
                if (open) ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::BeginChild("area", ImVec2(0, 0), ImGuiChildFlags_None);
        switch (app.area) {
        case Area::Playlist: drawPlaylist(app); break;
        case Area::Channels: drawChannels(app); break;
        case Area::PianoRoll: drawPianoRoll(app); break;
        case Area::Mixer: drawMixer(app); break;
        case Area::Vocals: drawVocals(app); break;
        case Area::Beats: drawBeats(app); break;
        case Area::Plugins: drawPlugins(app); break;
        case Area::Master: drawMaster(app); break;
        case Area::Project: drawProject(app); break;
        default: break;
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    statusBar(app);
    ImGui::End();
    drawPalette(app);
}

// ---------------------------------------------------------------- command palette
void drawPalette(App& app) {
    static char query[128] = "";
    static char args[1024] = "{}";
    static std::string selected;
    if (app.showPalette) {
        ImGui::OpenPopup("Command Palette");
        app.showPalette = false;
        query[0] = 0;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 80), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(620, 0));
    if (!ImGui::BeginPopupModal("Command Palette", nullptr, ImGuiWindowFlags_NoResize)) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", "type a command...", query, sizeof(query));
    ImGui::BeginChild("results", ImVec2(0, 260), ImGuiChildFlags_Borders);
    for (auto* c : app.registry().search(query)) {
        const bool sel = selected == c->id;
        if (ImGui::Selectable(std::format("{:<22}  {}", c->title, c->category).c_str(), sel)) selected = c->id;
        if (!c->shortcut.empty()) {
            ImGui::SameLine(520);
            ImGui::TextDisabled("%s", c->shortcut.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Arguments (JSON)");
    ImGui::InputTextMultiline("##args", args, sizeof(args), ImVec2(-1, 60));
    if (goldButton("Run") && !selected.empty()) {
        json a = json::parse(args, nullptr, false);
        if (a.is_discarded() || !a.is_object()) app.message(1, "arguments must be a JSON object");
        else if (app.run(selected, a)) app.message(0, selected + " done");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace roy::gui
