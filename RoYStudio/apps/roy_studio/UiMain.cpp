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

void shortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) app.togglePlay();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? app.redo() : app.undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) app.redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) app.save();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) app.showPalette = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_B, false) && app.hasProject()) app.pickAndImportBeat();
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
        if (ImGui::MenuItem("Import beat (MP3 / WAV)...", "Ctrl+B", false, app.hasProject())) app.pickAndImportBeat();
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
        if (ImGui::MenuItem("Setup check (audio / MIDI / plugins)...")) {
            app.setupStep = 0;
            app.showFirstRun = true;
        }
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
        if (ImGui::MenuItem("FIRST REAL MUSIC SESSION (guided test)...")) app.showMusicSession = true;
        if (ImGui::MenuItem("Setup check (audio / MIDI / plugins)...")) {
            app.setupStep = 0;
            app.showFirstRun = true;
        }
        if (ImGui::MenuItem("System check...")) {
            app.runSystemCheck();
            app.showSystemCheck = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("CREATE DIAGNOSTIC PACKAGE (zip)")) app.createDiagnosticPackage();
        if (ImGui::MenuItem("Create diagnostics report (text only)")) app.createDiagnosticsReport();
        if (ImGui::MenuItem("Open user data folder (logs, settings)")) app.openFolder(files::userDataDirectory());
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
        if (!g_newFolder[0]) std::snprintf(g_newFolder, sizeof(g_newFolder), "%s", files::defaultProjectsDirectory().string().c_str());
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
    // CRACKLE HELP: the two usual causes, each with a one-click answer
    {
        static uint64_t lastOvers = 0, lastXruns = 0;
        static double oversUntil = 0, xrunsUntil = 0;
        const double now = ImGui::GetTime();
        if (st.outputOvers > lastOvers) oversUntil = now + 4.0;
        if (st.overloads > lastXruns && app.engine().transport().isPlaying()) xrunsUntil = now + 6.0;
        lastOvers = st.outputOvers;
        lastXruns = st.overloads;
        if (now < oversUntil) {
            ImGui::SameLine();
            if (toggleButton("TOO LOUD - lower the BEAT fader", true, col::Red)) app.area = Area::Mixer;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The master goes over 0 dB (red CLIP in the MIXER). RoY softens it so it does not crack,\n"
                                  "but it sounds best with headroom: MIXER > beat track fader to about -6 dB. Click = open MIXER.");
        }
        const int buf = app.audioConfig().bufferSize;
        if (now < xrunsUntil && buf < 1024) {
            ImGui::SameLine();
            const int next = buf < 512 ? 512 : 1024;
            if (toggleButton(std::format("DROPOUTS (crackles) - click: buffer {}", next).c_str(), true, col::Orange)) {
                AudioDeviceConfig cfg = app.audioConfig();
                cfg.bufferSize = next;
                app.changeAudio(cfg);
                xrunsUntil = 0;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The computer did not finish the audio in time (xruns). A bigger buffer fixes it\n"
                                  "(a little more delay when you hear yourself live). Audio > Buffer to change it back.");
        }
    }
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
    if (ImGui::Button("Demo Project", ImVec2(200, 0))) app.buildDemo(files::defaultProjectsDirectory());
    ImGui::EndGroup();
}
} // namespace

bool tempoField(App& app, const char* id, float width, bool stepButtons) {
    Project& p = app.project();
    // The project tempo is re-read every frame, so the value being dragged/typed is kept here until
    // the edit ends - otherwise a drag would snap back to the old tempo each frame.
    static ImGuiID editing = 0;
    static float editBpm = 0;
    const float current = static_cast<float>(p.tempo.tempoAt(0));
    auto apply = [&](double bpm) {
        bpm = std::clamp(std::round(bpm * 10.0) / 10.0, 10.0, 999.0);
        if (std::abs(bpm - current) < 0.05) return false;
        return app.run("SetTempo", {{"bpm", bpm}, {"atBeat", 0.0}});
    };
    bool changed = false;
    ImGui::PushID(id);
    const float bw = ImGui::GetFrameHeight();
    if (stepButtons) {
        if (ImGui::Button("-", ImVec2(bw, 0))) changed |= apply(current - 1.0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("tempo -1 BPM");
        ImGui::SameLine(0, 2);
    }
    const ImGuiID wid = ImGui::GetID("##tempo");
    float bpm = editing == wid ? editBpm : current;
    ImGui::SetNextItemWidth(width);
    ImGui::DragFloat("##tempo", &bpm, 0.2f, 10.0f, 999.0f, "%.1f BPM", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActive()) {
        editing = wid;
        editBpm = bpm;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed |= apply(bpm);
    if (ImGui::IsItemDeactivated() && editing == wid) editing = 0;
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) changed |= apply(current + (wheel > 0 ? 1.0 : -1.0) * (ImGui::GetIO().KeyShift ? 0.1 : 1.0));
        ImGui::SetTooltip("Tempo: drag left/right, double-click to type a value,\nmouse wheel +-1 BPM (Shift: +-0.1). Ctrl+Z undoes.");
    }
    if (stepButtons) {
        ImGui::SameLine(0, 2);
        if (ImGui::Button("+", ImVec2(bw, 0))) changed |= apply(current + 1.0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("tempo +1 BPM");
    }
    ImGui::PopID();
    return changed;
}

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
        if (goldButton("EXPORT", ImVec2(bs.x * 1.3f, bs.y))) app.area = Area::Master; // WAV / MP3 / FLAC
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export the song as WAV / MP3 / FLAC (MASTER page)");
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
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
        tempoField(app, "##bpm", 90 * ImGui::GetFontSize() / 15.0f, true);
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
    if (app.exportRunning()) { // the project must not change while it is being exported
        drawExportProgress(app);
        return;
    }
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
    drawSetupWizard(app);
    drawSystemCheck(app);
    drawMusicSession(app);
    drawImportBeat(app);
    drawExportResult(app);
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
    app.handleDroppedFiles(); // files dropped from Explorer that the PLAYLIST did not place on a track
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
