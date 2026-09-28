#include "Ui.h"

#include "core/Files.h"

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
        for (int b : {64, 128, 256, 512, 1024})
            if (ImGui::MenuItem(std::format("Buffer {} samples", b).c_str(), nullptr, cfg.bufferSize == b)) {
                cfg.bufferSize = b;
                app.restartAudio(cfg);
            }
        ImGui::Separator();
        for (double sr : {44100.0, 48000.0, 96000.0})
            if (ImGui::MenuItem(std::format("{:.0f} Hz", sr).c_str(), nullptr, cfg.sampleRate == sr)) {
                cfg.sampleRate = sr;
                app.restartAudio(cfg);
            }
        ImGui::Separator();
        ImGui::TextDisabled("MIDI INPUTS");
        if (auto* mi = app.midiInput()) {
            const auto devs = mi->devices();
            if (devs.empty()) ImGui::TextDisabled("no MIDI input found - connect a keyboard");
            for (auto& d : devs) {
                const bool open = mi->isOpen(d.id);
                if (ImGui::MenuItem(d.name.c_str(), nullptr, open)) {
                    std::string err;
                    if (open) mi->close(d.id);
                    else if (!mi->open(d.id, &err)) app.message(2, err);
                }
            }
            ImGui::TextDisabled("last: %s", mi->lastMessageText().c_str());
            const std::string target = app.liveMidiTrackName();
            ImGui::TextDisabled("plays: %s", target.empty() ? "(select a track with an instrument)" : target.c_str());
            if (ImGui::MenuItem("MIDI panic (all notes off)")) app.midiPanic();
        }
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
