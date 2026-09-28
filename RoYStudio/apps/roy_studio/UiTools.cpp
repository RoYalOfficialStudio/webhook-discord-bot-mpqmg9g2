// VOCALS (Vocal Lab), PLUGINS (manager + sandbox status), BROWSER, PROJECT.
#include "Ui.h"

#include "core/Files.h"
#include "plugins/Sandbox.h"
#include "project/ProjectIO.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace roy::gui {

namespace {
void issueList(App& app, const json& issues, const char* fixesKey) {
    int k = 0;
    for (auto& is : issues) {
        ImGui::PushID(k++);
        const std::string sev = is.value("severity", "info");
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sev == "problem" || sev == "high" ? col::Red : sev == "warning" || sev == "medium" ? col::Orange : col::Green),
                           "%s", is.value("title", is.value("text", "")).c_str());
        if (is.contains("detail")) ImGui::TextDisabled("   %s", is.value("detail", "").c_str());
        int n = 0;
        for (auto& f : is.value(fixesKey, json::array())) {
            ImGui::PushID(n++);
            ImGui::TextDisabled("   -> %s", f.value("description", "").c_str());
            if (!f.value("command", "").empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("fix")) app.run(f["command"].get<std::string>(), f.value("args", json::object()));
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}
} // namespace

// ---------------------------------------------------------------- VOCALS
void drawVocals(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    Track* owner = nullptr;
    AudioClip* clip = p.findAudioClip(app.selClip, &owner);
    ImGui::BeginChild("vocalLeft", ImVec2(360 * dpi, 0), ImGuiChildFlags_Borders);
    sectionTitle("VOCAL CLIPS");
    for (auto& t : p.tracks) {
        if (t.type != TrackType::Audio) continue;
        for (auto& c : t.audioClips)
            if (ImGui::Selectable(std::format("{}  /  {}  @ {:.1f}", t.name, c.name, c.startBeat + 1).c_str(), c.id == app.selClip)) {
                app.selClip = c.id;
                app.vocalResult = json::object();
            }
    }
    if (!clip) {
        ImGui::TextDisabled("Select a vocal clip.");
        ImGui::EndChild();
        return;
    }
    ImGui::Spacing();
    sectionTitle("PITCH GUARDIAN");
    static int mode = 1;
    static float strength = 0.8f, speed = 25.0f, humanize = 0.2f;
    static bool offKey = true, formant = true, chromatic = false;
    const char* modes[] = {"warn", "assist", "lock"};
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Mode", &mode, modes, 3);
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::SliderFloat("Strength", &strength, 0, 1);
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::SliderFloat("Speed", &speed, 1, 200, "%.0f ms");
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::SliderFloat("Humanize", &humanize, 0, 1);
    ImGui::Checkbox("OFF-KEY FILTER", &offKey);
    ImGui::SameLine();
    ImGui::Checkbox("Formants", &formant);
    ImGui::Checkbox("Allow chromatic passing notes", &chromatic);
    ImGui::TextDisabled("Key: %s  (change in PROJECT)", p.key.name().c_str());
    if (goldButton("Apply Pitch Guardian", ImVec2(-1, 0))) {
        if (app.run("PitchGuardian", {{"clipId", clip->id}, {"mode", modes[mode]}, {"strength", strength}, {"speedMs", speed}, {"humanize", humanize},
                                      {"offKeyFilter", offKey}, {"formantPreserve", formant}, {"allowChromatic", chromatic}})) {
            app.vocalResult = app.lastResult();
            app.vocalResult["kind"] = "guardian";
            app.message(0, std::format("Pitch Guardian: {} notes corrected (original audio kept, undo restores it)", app.vocalResult.value("corrected", 0)));
        }
    }
    ImGui::Spacing();
    sectionTitle("ANALYSIS & REPAIR");
    if (ImGui::Button("Pitch analysis", ImVec2(-1, 0)) && app.run("PitchAnalysis", {{"clipId", clip->id}})) {
        app.vocalResult = app.lastResult();
        app.vocalResult["kind"] = "pitch";
    }
    if (ImGui::Button("Vocal Doctor", ImVec2(-1, 0)) && app.run("VocalDoctor", {{"clipId", clip->id}})) {
        app.vocalResult = app.lastResult();
        app.vocalResult["kind"] = "doctor";
    }
    if (ImGui::Button("Flow analyzer (timing vs beat)", ImVec2(-1, 0)) && app.run("FlowAnalyze", {{"clipId", clip->id}})) {
        app.vocalResult = app.lastResult();
        app.vocalResult["kind"] = "flow";
    }
    if (ImGui::Button("Reduce breaths (-12 dB)", ImVec2(-1, 0))) app.run("BreathReduce", {{"clipId", clip->id}, {"reductionDb", -12.0}});
    if (ImGui::Button("Learn my Vocal DNA from this take", ImVec2(-1, 0))) app.run("LearnVocalDna", {{"clipId", clip->id}});
    // Double Magnet: align another clip on a track with role "double"/"adlib" to this one
    static std::string doubleClip;
    if (ImGui::BeginCombo("Double", doubleClip.empty() ? "choose double..." : doubleClip.c_str())) {
        for (auto& t : p.tracks)
            for (auto& c : t.audioClips)
                if (c.id != clip->id && ImGui::Selectable((t.name + " / " + c.name).c_str())) doubleClip = c.id;
        ImGui::EndCombo();
    }
    if (!doubleClip.empty() && ImGui::Button("DOUBLE MAGNET (align timing + pitch)", ImVec2(-1, 0)))
        app.run("DoubleMagnet", {{"mainClipId", clip->id}, {"doubleClipId", doubleClip}, {"timing", 1.0}, {"pitch", true}});
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("vocalRight", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const json& r = app.vocalResult;
    const std::string kind = r.is_object() ? r.value("kind", "") : "";
    if (kind == "pitch" || kind == "guardian") {
        sectionTitle(kind == "pitch" ? "PITCH ANALYSIS" : "PITCH GUARDIAN RESULT");
        if (kind == "pitch") ImGui::Text("In tune: %.0f %%   rap indicator %.2f", r.value("inTuneRatio", 0.0) * 100, r.value("rapIndicator", 0.0));
        // note graph: each note as a bar at its pitch, colour = cents deviation
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float W = ImGui::GetContentRegionAvail().x, H = 220 * dpi;
        dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), col::Obsidian, 4);
        const json notes = r.value("notes", json::array());
        double t1 = 0, lo = 127, hi = 0;
        for (auto& n : notes) {
            t1 = std::max(t1, n.value("end", 0.0));
            const double m = n.value("midi", n.value("from", 60.0));
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        lo = std::floor(lo) - 2;
        hi = std::ceil(hi) + 2;
        for (int k = static_cast<int>(lo); k <= static_cast<int>(hi); ++k) {
            const float y = o.y + H - static_cast<float>((k - lo) / std::max(1.0, hi - lo)) * H;
            dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + W, y), p.key.contains(k) ? col::rgb(0x2E2A1C) : col::rgb(0x18181C));
        }
        int i = 0;
        for (auto& n : notes) {
            const double m = n.value("midi", n.value("from", 60.0));
            const double s = n.value("start", static_cast<double>(i)), e = n.value("end", s + 0.8);
            const double cents = n.value("cents", 0.0);
            const float x0 = o.x + static_cast<float>(s / std::max(1e-6, t1 > 0 ? t1 : notes.size() + 1.0)) * W;
            const float x1 = o.x + static_cast<float>(e / std::max(1e-6, t1 > 0 ? t1 : notes.size() + 1.0)) * W;
            const float y = o.y + H - static_cast<float>((m - lo) / std::max(1.0, hi - lo)) * H;
            const ImU32 c = std::fabs(cents) < 15 ? col::Green : std::fabs(cents) < 35 ? col::Orange : col::Red;
            dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(std::max(x0 + 3, x1), y + 3), kind == "guardian" ? col::Gold : c, 2);
            ++i;
        }
        ImGui::Dummy(ImVec2(W, H));
        if (ImGui::BeginTable("notes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("time");
            ImGui::TableSetupColumn("note");
            ImGui::TableSetupColumn("cents");
            ImGui::TableSetupColumn("kind");
            ImGui::TableSetupColumn("confidence");
            ImGui::TableHeadersRow();
            for (auto& n : notes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.2f s", n.value("start", 0.0));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.value("note", n.contains("to") ? std::format("{:.2f} -> {:.2f}", n.value("from", 0.0), n.value("to", 0.0)) : "").c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%+.0f", n.value("cents", n.value("shift", 0.0) * 100));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.value("kind", n.value("reason", "")).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", n.value("confidence", 1.0));
            }
            ImGui::EndTable();
        }
        for (auto& w : r.value("issues", r.value("warnings", json::array()))) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "%s", w.get<std::string>().c_str());
    } else if (kind == "doctor") {
        sectionTitle("VOCAL DOCTOR");
        const json m = r.value("measurements", json::object());
        ImGui::Text("Peak %.1f dBFS | RMS %.1f dB | SNR %.0f dB | noise floor %.0f dB | clips %d | sibilants %d | plosives %d | in tune %.0f %%",
                    m.value("peakDb", 0.0), m.value("rmsDb", 0.0), m.value("snrDb", 0.0), m.value("noiseFloorDb", 0.0), m.value("clipEvents", 0),
                    m.value("sibilantEvents", 0), m.value("plosiveEvents", 0), m.value("inTuneRatio", 0.0) * 100);
        issueList(app, r.value("issues", json::array()), "fixes");
    } else if (kind == "flow") {
        sectionTitle("FLOW ANALYZER");
        ImGui::TextWrapped("%s", r.dump(2).c_str());
    } else {
        ImGui::TextDisabled("Run an analysis on the left. Every change creates a new audio file - the original recording is never modified.");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- PLUGINS
void drawPlugins(App& app) {
    const float dpi = ImGui::GetFontSize() / 15.0f;
    static int view = 1;
    const char* views[] = {"AVAILABLE", "INSTRUMENTS", "EFFECTS", "VST3", "CLAP", "FAVORITES", "RECENT", "FAILED", "BLACKLISTED", "DUPLICATES", "INSTALLED"};
    const int nViews = 11;
    static char psearch[96] = "";
    if (app.scanning()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "Scanning plugins out of process...");
    } else {
        if (goldButton("Scan")) app.startPluginScan(false, false);
        ImGui::SameLine();
        if (ImGui::Button("Full rescan")) app.startPluginScan(true, false);
        ImGui::SameLine();
        if (ImGui::Button("Retry quarantined")) app.startPluginScan(true, true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.scanSummary().c_str());
    for (int i = 0; i < nViews; ++i) {
        if (i) ImGui::SameLine();
        if (toggleButton(views[i], view == i, col::Gold)) view = i;
    }
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##plugsearch", "search name, vendor, tag...", psearch, sizeof(psearch));
    auto& db = app.pluginDb();
    const std::string vname = views[view];
    json all = db.view(vname == "VST3" || vname == "CLAP" ? "AVAILABLE" : vname);
    json rows = json::array();
    std::string q = psearch;
    std::transform(q.begin(), q.end(), q.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    for (auto& r : all) {
        if (vname == "VST3" && r.value("format", "") != "vst3") continue;
        if (vname == "CLAP" && r.value("format", "") != "clap") continue;
        std::string hay = r.value("name", "") + " " + r.value("vendor", "") + " " + r.value("category", "");
        for (auto& f : r.value("features", json::array())) hay += " " + f.get<std::string>();
        std::transform(hay.begin(), hay.end(), hay.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (!q.empty() && hay.find(q) == std::string::npos) continue;
        rows.push_back(r);
    }
    const float tableH = ImGui::GetContentRegionAvail().y * 0.6f;
    if (ImGui::BeginTable("plugins", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable,
                          ImVec2(0, tableH))) {
        for (const char* h : {"Name", "Vendor", "Version", "Format", "Type", "Arch", "Status", ""}) ImGui::TableSetupColumn(h);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int k = 0;
        for (auto& r : rows) {
            ImGui::PushID(k++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Selectable(r.value("name", "").c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            if (r.value("status", "") == "ok" && ImGui::BeginDragDropSource()) {
                const std::string payload = r.value("typeId", "") + "\n" + r.value("name", "");
                ImGui::SetDragDropPayload("ROY_PLUGIN", payload.data(), payload.size());
                ImGui::Text("%s -> drop on a mixer strip", r.value("name", "").c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && !r.value("path", "").empty()) ImGui::SetTooltip("%s", r.value("path", "").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.value("vendor", "").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.value("version", "").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.value("format", "").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.value("category", "").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.value("arch", "").c_str());
            ImGui::TableNextColumn();
            const std::string st = r.value("status", "");
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st == "ok" ? col::Green : st == "unsupported" ? col::IvoryDim : col::Red), "%s%s", st.c_str(),
                               r.value("quarantined", false) ? " (quarantined)" : "");
            if (ImGui::IsItemHovered() && !r.value("error", "").empty()) ImGui::SetTooltip("%s", r.value("error", "").c_str());
            ImGui::TableNextColumn();
            const std::string tid = r.value("typeId", ""), path = r.value("path", "");
            if (st == "ok") {
                if (ImGui::SmallButton(r.value("favorite", false) ? "unfav" : "fav")) {
                    db.setFavorite(tid, !r.value("favorite", false));
                    app.savePluginDb();
                }
                ImGui::SameLine();
                if (r.value("category", "") == "effect" && !app.selChannel.empty() && ImGui::SmallButton("insert")) {
                    if (app.run("AddInsert", {{"channelId", app.selChannel}, {"typeId", tid}, {"name", r.value("name", "")}})) db.markUsed(tid);
                    app.savePluginDb();
                }
                if (r.value("category", "") == "instrument" && !app.selTrack.empty() && ImGui::SmallButton("use")) {
                    if (app.run("SetInstrument", {{"trackId", app.selTrack}, {"typeId", tid}, {"name", r.value("name", "")}})) db.markUsed(tid);
                    app.savePluginDb();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(r.value("blacklisted", false) ? "unblock" : "block")) {
                db.setBlacklisted(path, !r.value("blacklisted", false));
                app.savePluginDb();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Every VST3 / CLAP plugin runs in its own RoYPluginHost process: a crash never takes RoY Studio down.");
    sectionTitle("RUNNING PLUGINS");
    if (app.hasProject() && ImGui::BeginTable("running", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        for (const char* h : {"Plugin", "Where", "Host PID", "Status", ""}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (auto& [slot, proc] : app.runtime().allProcessors()) {
            auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get());
            if (!sp) continue;
            ImGui::PushID(slot.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sp->displayName().c_str());
            ImGui::TableNextColumn();
            MixerChannel* owner = nullptr;
            ImGui::TextUnformatted(app.project().findSlot(slot, &owner) && owner ? owner->name.c_str() : "instrument");
            ImGui::TableNextColumn();
            ImGui::Text("%d", sp->hostPid());
            ImGui::TableNextColumn();
            if (sp->alive()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "running");
            else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "PLUGIN CRASHED - %s", sp->problem().c_str());
            ImGui::TableNextColumn();
            if (sp->alive()) {
                if (ImGui::SmallButton("editor")) app.run("OpenPluginEditor", {{"slotId", slot}});
            } else {
                if (ImGui::SmallButton("restart")) app.run("RestartPlugin", {{"slotId", slot}});
                ImGui::SameLine();
                if (ImGui::SmallButton("disable")) app.run("BypassInsert", {{"slotId", slot}, {"bypass", true}});
                ImGui::SameLine();
                if (ImGui::SmallButton("remove")) app.run("RemoveInsert", {{"slotId", slot}});
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (auto& c : app.crashes()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "%s  %s: %s", c.time.c_str(), c.pluginName.c_str(), c.reason.c_str());
    (void)dpi;
}

// ---------------------------------------------------------------- BROWSER
void drawBrowser(App& app) {
    static char filter[128] = "";
    static char rootBuf[1024] = "";
    static int tab = 0;
    static std::vector<std::pair<fs::path, bool>> entries;
    static fs::path listed;
    static fs::path selected;
    sectionTitle("BROWSER");
    const char* tabs[] = {"Project", "Samples", "Drums", "808", "Loops", "Vocals", "Home"};
    for (int i = 0; i < 7; ++i) {
        if (i && i != 4) ImGui::SameLine();
        if (toggleButton(tabs[i], tab == i, col::Gold)) {
            tab = i;
            rootBuf[0] = 0;
        }
    }
    if (!rootBuf[0]) {
        fs::path root;
        if (tab == 0 && app.hasProject()) root = app.session().folder();
        else if (tab >= 1 && tab <= 5) root = browser::categoryFolder(tabs[tab]);
        else {
            const char* h = std::getenv(
#ifdef _WIN32
                "USERPROFILE"
#else
                "HOME"
#endif
            );
            root = h ? fs::path(h) : fs::current_path();
        }
        std::snprintf(rootBuf, sizeof(rootBuf), "%s", root.string().c_str());
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##root", rootBuf, sizeof(rootBuf));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search...", filter, sizeof(filter));
    // preview controls
    ImGui::Checkbox("Auto", &app.previewAuto);
    ImGui::SameLine();
    ImGui::Checkbox("Tempo sync", &app.previewTempoSync);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##pvol", &app.previewVolume, 0.0f, 1.0f, "Preview %.2f")) app.previewer().setGain(app.previewVolume);
    const bool playing = app.previewer().playing();
    if (toggleButton(playing ? "STOP" : "PREVIEW", playing, col::Green, ImVec2(-1, 0))) {
        if (playing) app.previewer().stop();
        else if (!selected.empty()) app.previewFile(selected);
    }
    if (playing && app.lastPreview.ok)
        ImGui::TextDisabled("%s  %.1f s%s", app.previewer().current().filename().string().c_str(), app.lastPreview.seconds,
                            app.lastPreview.sourceBpm > 0 ? std::format("  {:.0f} BPM ({})", app.lastPreview.sourceBpm, app.lastPreview.bpmSource).c_str() : "");
    const fs::path root(rootBuf);
    if (listed != root || ImGui::GetFrameCount() % 120 == 0) {
        entries.clear();
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            entries.push_back({it->path(), it->is_directory(ec)});
            if (entries.size() > 2000) break;
        }
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        listed = root;
    }
    ImGui::BeginChild("files", ImVec2(0, 0), ImGuiChildFlags_None);
    if (root.has_parent_path() && root.parent_path() != root && ImGui::Selectable(".. (up)")) std::snprintf(rootBuf, sizeof(rootBuf), "%s", root.parent_path().string().c_str());
    std::string f = filter;
    std::transform(f.begin(), f.end(), f.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    for (auto& [path, dir] : entries) {
        std::string name = path.filename().string();
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (!f.empty() && low.find(f) == std::string::npos) continue;
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        const bool audio = ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".aif" || ext == ".aiff" || ext == ".ogg";
        const bool project = ext == ".roy";
        const bool midi = ext == ".mid" || ext == ".midi";
        if (!dir && !audio && !project && !midi) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(dir ? col::Gold : project ? col::Orange : audio ? col::Ivory : col::Green));
        const bool isPlaying = playing && app.previewer().current() == path;
        const std::string label = (dir ? "[+] " : audio ? (isPlaying ? "> " : "~ ") : project ? "* " : "# ") + name;
        if (ImGui::Selectable(label.c_str(), selected == path, ImGuiSelectableFlags_AllowDoubleClick)) {
            selected = path;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (dir) std::snprintf(rootBuf, sizeof(rootBuf), "%s", path.string().c_str());
                else if (project) app.openProject(path);
                else if (audio && app.hasProject()) {
                    std::string tid = app.selTrack;
                    const Track* t = app.project().findTrack(tid);
                    if (!t || t->type != TrackType::Audio) {
                        app.run("AddTrack", {{"type", "audio"}, {"name", path.stem().string()}});
                        tid = app.lastResult().value("id", "");
                    }
                    app.run("ImportAudio", {{"path", path.string()}, {"trackId", tid}, {"startBeat", app.positionBeats()}});
                } else if (midi && app.hasProject() && !app.selTrack.empty())
                    app.run("ImportMidi", {{"path", path.string()}, {"trackId", app.selTrack}});
            } else if (audio) {
                // one click: preview; click the playing file again: stop
                if (isPlaying) app.previewer().stop();
                else if (app.previewAuto) app.previewFile(path);
            }
        }
        ImGui::PopStyleColor();
        if (audio && ImGui::BeginDragDropSource()) {
            const std::string s = path.string();
            ImGui::SetDragDropPayload("ROY_FILE", s.data(), s.size());
            ImGui::Text("%s", name.c_str());
            ImGui::EndDragDropSource();
        }
    }
    ImGui::TextDisabled("click: preview | double-click: import at playhead | drag onto playlist, drum pad, sampler lane or mixer strip");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- PROJECT
void drawProject(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginChild("projLeft", ImVec2(420 * dpi, 0), ImGuiChildFlags_Borders);
    sectionTitle("SONG");
    static char name[128];
    if (ImGui::IsWindowAppearing() || !name[0]) std::snprintf(name, sizeof(name), "%s", p.name.c_str());
    ImGui::SetNextItemWidth(220 * dpi);
    ImGui::InputText("Name", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("RenameProject", {{"name", std::string(name)}});
    float bpm = static_cast<float>(p.tempo.tempoAt(0));
    ImGui::SetNextItemWidth(220 * dpi);
    ImGui::DragFloat("Tempo", &bpm, 0.1f, 40, 250, "%.1f BPM");
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetTempo", {{"bpm", bpm}, {"atBeat", 0.0}});
    static int root = 9, scale = 1;
    const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    root = p.key.root;
    ImGui::SetNextItemWidth(80 * dpi);
    if (ImGui::Combo("##root", &root, roots, 12)) app.run("SetKey", {{"key", std::string(roots[root]) + " " + p.key.name().substr(p.key.name().find(' ') + 1)}});
    ImGui::SameLine();
    const auto scales = allScaleTypes();
    ImGui::SetNextItemWidth(132 * dpi);
    if (ImGui::BeginCombo("Key", scaleTypeName(p.key.scale))) {
        for (auto s : scales)
            if (ImGui::Selectable(scaleTypeName(s), s == p.key.scale)) app.run("SetKey", {{"key", std::string(roots[p.key.root]) + " " + scaleTypeName(s)}});
        ImGui::EndCombo();
    }
    (void)scale;
    int num = p.tempo.signatureAtBar(0).numerator, den = p.tempo.signatureAtBar(0).denominator;
    ImGui::SetNextItemWidth(100 * dpi);
    if (ImGui::InputInt("Beats/bar", &num) && num >= 1 && num <= 32) app.run("SetTimeSignature", {{"numerator", num}, {"denominator", den}});
    bool metro = p.settings.metronome;
    if (ImGui::Checkbox("Metronome", &metro)) app.run("SetMetronome", {{"enabled", metro}});
    int countIn = p.settings.countInBars;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * dpi);
    if (ImGui::InputInt("Count-in bars", &countIn) && countIn >= 0 && countIn <= 4) app.run("SetMetronome", {{"enabled", metro}, {"countInBars", countIn}});
    ImGui::Spacing();
    sectionTitle("FILE");
    ImGui::TextWrapped("%s", app.session().file().string().c_str());
    if (goldButton("Save")) app.save();
    ImGui::SameLine();
    ImGui::TextDisabled(app.dirty() ? "unsaved changes (autosave every 60 s into RECOVERY/)" : "all changes saved");
    const auto backups = listBackups(app.session().file());
    ImGui::Text("Backups: %zu (kept: %d)", backups.size(), p.settings.backupRetention);
    ImGui::Spacing();
    sectionTitle("HISTORY");
    auto& u = app.undoManager();
    ImGui::Text("Undo steps: %zu  |  Redo: %zu  |  memory %.1f MB", u.undoCount(), u.redoCount(), u.memoryBytes() / 1048576.0);
    if (ImGui::Button("Undo") && u.canUndo()) app.undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo") && u.canRedo()) app.redo();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", u.canUndo() ? ("next undo: " + u.undoName()).c_str() : "");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("projRight", ImVec2(0, 0), ImGuiChildFlags_Borders);
    sectionTitle("PROJECT ASSISTANT");
    if (goldButton("Check project")) {
        if (app.run("ProjectCheck", {{"dirty", app.dirty()}, {"backups", backups.size()}})) app.checkResult = app.lastResult();
    }
    int k = 0;
    for (auto& f : app.checkResult.value("findings", json::array())) {
        ImGui::PushID(k++);
        const std::string sev = f.value("severity", "info");
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sev == "problem" ? col::Red : sev == "warning" ? col::Orange : col::IvoryDim), "%s", f.value("text", "").c_str());
        if (!f.value("command", "").empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("fix")) {
                app.run(f["command"].get<std::string>(), f.value("args", json::object()));
                app.checkResult = json::object();
            }
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    sectionTitle("ARRANGEMENT");
    if (ImGui::Button("Suggest sections from energy")) {
        if (app.run("EnergyMap", {{"startBeat", 0.0}, {"endBeat", std::max(8.0, p.endBeat())}})) {
            json secs = app.lastResult().value("sections", json::array());
            if (!secs.empty()) app.run("ApplySections", {{"sections", secs}, {"replace", true}});
        }
    }
    for (auto& s : p.sections) ImGui::BulletText("%s (%s)  bars %.0f - %.0f", s.name.c_str(), s.type.c_str(), s.startBeat / 4 + 1, s.endBeat / 4 + 1);
    ImGui::Spacing();
    sectionTitle("LOG");
    for (auto it = app.log.rbegin(); it != app.log.rend(); ++it)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(it->level >= 2 ? col::Red : it->level == 1 ? col::Orange : col::IvoryDim), "%s", it->text.c_str());
    ImGui::EndChild();
}

} // namespace roy::gui
