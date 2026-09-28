// MIXER (channel strips, inserts, sends, parameter editor) and MASTER (chain, loudness, export).
#include "Ui.h"

#include "plugins/Sandbox.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::gui {

namespace {
// Generic parameter editor for any processor (built-in or sandboxed plugin):
// search, favourites (stored as UI data in the slot), last touched parameter -> automation.
void paramEditor(App& app, const std::string& slotId) {
    auto proc = app.runtime().processorForSlot(slotId);
    if (!proc) {
        if (app.runtime().safeMode()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "SAFE MODE: plugin not loaded (state kept)");
        else ImGui::TextDisabled("processor not available");
        return;
    }
    const float dpi = ImGui::GetFontSize() / 15.0f;
    MixerChannel* owner = nullptr;
    PluginSlot* slot = app.project().findSlot(slotId, &owner);
    if (auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get())) {
        if (!sp->alive()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "PLUGIN CRASHED: %s", sp->problem().c_str());
            if (goldButton("RESTART")) app.run("RestartPlugin", {{"slotId", slotId}});
            ImGui::SameLine();
            if (ImGui::Button("DISABLE")) app.run("BypassInsert", {{"slotId", slotId}, {"bypass", true}});
            ImGui::SameLine();
            if (ImGui::Button("REMOVE")) app.run("RemoveInsert", {{"slotId", slotId}});
            ImGui::TextDisabled("The project keeps playing: the slot passes audio through. Reopen with File > Open (Safe Mode) if a plugin keeps crashing.");
            return;
        }
        ImGui::TextDisabled("%s plugin in sandbox (pid %d) | latency %d samples", sp->format() == "vst3" ? "VST3" : "CLAP", sp->hostPid(), sp->latencySamples());
        static bool onTop = false;
        if (goldButton("OPEN EDITOR")) app.run("OpenPluginEditor", {{"slotId", slotId}, {"alwaysOnTop", onTop}});
        ImGui::SameLine();
        if (ImGui::Button("Close editor")) app.run("ClosePluginEditor", {{"slotId", slotId}});
        ImGui::SameLine();
        ImGui::Checkbox("Always on top", &onTop);
        const int lt = sp->lastTouchedParam();
        if (lt >= 0 && lt < proc->numParams()) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "Last touched: %s", proc->paramInfo(lt).name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Automate") && owner) {
                const double b = app.positionBeats();
                const float v = proc->getParam(lt);
                app.run("CreateAutomation", {{"channelId", owner->id}, {"slotId", slotId}, {"paramId", proc->paramInfo(lt).id}, {"points", {{b, v}, {b + 4.0, v}}}});
            }
        }
    }
    static char search[64] = "";
    ImGui::SetNextItemWidth(200 * dpi);
    ImGui::InputTextWithHint("##psearch", "search parameters...", search, sizeof(search));
    std::string q = search;
    std::transform(q.begin(), q.end(), q.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    json favs = slot && slot->state.is_object() && slot->state.contains("ui") ? slot->state["ui"].value("favorites", json::array()) : json::array();
    auto isFav = [&](const std::string& id) {
        for (auto& f : favs)
            if (f.is_string() && f.get<std::string>() == id) return true;
        return false;
    };
    ImGui::BeginChild("params", ImVec2(0, 0));
    for (int pass = 0; pass < 2; ++pass) // favourites first
        for (int i = 0; i < proc->numParams(); ++i) {
            const auto& pi = proc->paramInfo(i);
            const bool fav = isFav(pi.id);
            if ((pass == 0) != fav) continue;
            std::string name = pi.name;
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (!q.empty() && name.find(q) == std::string::npos) continue;
            float v = proc->getParam(i);
            ImGui::PushID(i);
            if (slot && toggleButton(fav ? "*" : "+", fav, col::Gold, ImVec2(20 * dpi, 0))) {
                json nf = json::array();
                for (auto& f : favs)
                    if (!(f.is_string() && f.get<std::string>() == pi.id)) nf.push_back(f);
                if (!fav) nf.push_back(pi.id);
                app.run("SetSlotUi", {{"slotId", slotId}, {"key", "favorites"}, {"value", nf}});
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(240 * dpi);
            bool edited = false;
            if (pi.steps == 2) {
                bool b = v > 0.5f;
                if (ImGui::Checkbox(pi.name.c_str(), &b)) {
                    proc->setParam(i, b ? pi.maxValue : pi.minValue);
                    edited = true;
                }
            } else if (pi.steps > 2 && pi.maxValue - pi.minValue > 1.5f) {
                int k = static_cast<int>(std::lround(v));
                if (ImGui::SliderInt(pi.name.c_str(), &k, static_cast<int>(pi.minValue), static_cast<int>(pi.maxValue))) proc->setParam(i, static_cast<float>(k));
                edited = ImGui::IsItemDeactivatedAfterEdit();
            } else {
                if (ImGui::SliderFloat(pi.name.c_str(), &v, pi.minValue, pi.maxValue, pi.unit.empty() ? "%.3f" : ("%.3f " + pi.unit).c_str()))
                    proc->setParam(i, v); // live, lock-free
                edited = ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    proc->setParam(i, pi.defaultValue);
                    edited = true;
                }
            }
            if (edited) app.run("SetParam", {{"slotId", slotId}, {"paramId", pi.id}, {"value", proc->getParam(i)}});
            ImGui::PopID();
        }
    ImGui::TextDisabled("right-click a slider: default value | + / *: favourite");
    ImGui::EndChild();
}

void addInsertMenu(App& app, const std::string& channelId) {
    if (!ImGui::BeginPopup("addInsert")) return;
    sectionTitle("RoY EFFECTS");
    for (auto& e : ProcessorFactory::instance().entries()) {
        if (e.instrument) continue;
        if (ImGui::MenuItem(e.displayName.c_str())) app.run("AddInsert", {{"channelId", channelId}, {"typeId", e.typeId}, {"name", e.displayName}});
    }
    auto fx = app.pluginDb().effects();
    if (!fx.empty()) {
        sectionTitle("PLUGINS (VST3 / CLAP, sandboxed)");
        for (auto* r : fx)
            if (ImGui::MenuItem(std::format("{}  [{}]  {}", r->name, r->format == "vst3" ? "VST3" : "CLAP", r->vendor).c_str())) {
                if (app.run("AddInsert", {{"channelId", channelId}, {"typeId", r->typeId}, {"name", r->name}})) {
                    app.pluginDb().markUsed(r->typeId);
                    app.savePluginDb();
                }
            }
    }
    ImGui::EndPopup();
}

void strip(App& app, MixerChannel& ch, float width, float height, bool master) {
    const float dpi = ImGui::GetFontSize() / 15.0f;
    auto params = app.runtime().channelParams(ch.id);
    ImGui::PushID(ch.id.c_str());
    const bool sel = app.selChannel == ch.id;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(sel ? col::rgb(0x201D14) : col::Panel));
    ImGui::BeginChild("strip", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p0.x - 8, p0.y - 8), ImVec2(p0.x + width, p0.y - 4), clipColor(master ? 0xD4AF37 : ch.color));
    if (ImGui::Selectable(ch.name.c_str(), sel)) app.selChannel = ch.id;
    // inserts
    ImGui::TextDisabled("INSERTS");
    for (auto& s : ch.inserts) {
        ImGui::PushID(s.id.c_str());
        bool alive = true;
        if (auto proc = app.runtime().processorForSlot(s.id))
            if (auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get())) alive = sp->alive();
        if (toggleButton(s.bypass ? "o" : "*", !s.bypass, alive ? col::Green : col::Red, ImVec2(18 * dpi, 0)))
            app.run("BypassInsert", {{"slotId", s.id}, {"bypass", !s.bypass}});
        ImGui::SameLine();
        std::string label = s.name;
        if (!alive) label = "CRASHED " + label;
        if (ImGui::Selectable(label.c_str(), app.selSlot == s.id, 0, ImVec2(width - 40 * dpi, 0))) {
            app.selSlot = s.id;
            app.selChannel = ch.id;
        }
        if (ImGui::BeginPopupContextItem("slotMenu")) {
            if (ImGui::MenuItem("Remove")) app.run("RemoveInsert", {{"slotId", s.id}});
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (ch.inserts.size() < 32 && ImGui::Button("+ effect", ImVec2(-1, 0))) ImGui::OpenPopup("addInsert");
    addInsertMenu(app, ch.id);
    // sends
    if (!ch.sends.empty()) {
        ImGui::TextDisabled("SENDS");
        for (auto& s : ch.sends)
            if (const MixerChannel* t = app.project().findChannel(s.targetChannelId)) ImGui::TextDisabled("> %s %.0f dB", t->name.c_str(), s.levelDb);
    }
    // pan
    const float bottomH = height * 0.55f;
    ImGui::SetCursorPosY(height - bottomH);
    float pan = ch.pan;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##pan", &pan, -1, 1, pan == 0 ? "C" : pan < 0 ? "L %.2f" : "R %.2f") && params) params->pan.store(pan);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run(master ? "SetChannelPan" : "SetChannelPan", {{"channelId", ch.id}, {"master", master}, {"pan", pan}});
    // fader + meter
    float gain = ch.gainDb;
    const float faderH = bottomH - ImGui::GetFrameHeight() * 3.2f;
    ImGui::VSliderFloat("##fader", ImVec2(width * 0.42f, faderH), &gain, -60.0f, 12.0f, "%.1f dB");
    if (ImGui::IsItemActive() && params) params->gainDb.store(gain);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetChannelGain", {{"channelId", ch.id}, {"master", master}, {"gainDb", gain}});
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.run("SetChannelGain", {{"channelId", ch.id}, {"master", master}, {"gainDb", 0.0}});
    ImGui::SameLine();
    float pl = 0, pr = 0;
    if (params) {
        pl = params->peakL.exchange(0.0f);
        pr = params->peakR.exchange(0.0f);
    }
    levelMeter(pl, pr, ImVec2(width * 0.28f, faderH));
    if (params && params->clipCount.load() > 0) {
        ImGui::SameLine();
        if (toggleButton("CLIP", true, col::Red)) params->clipCount.store(0);
    }
    const ImVec2 bs((width - 24) / 3, 0);
    if (toggleButton("M", ch.mute, col::Orange, bs)) app.run("MuteChannel", {{"channelId", ch.id}, {"master", master}, {"mute", !ch.mute}});
    if (!master) {
        ImGui::SameLine();
        if (toggleButton("S", ch.solo, col::Gold, bs)) app.run("SoloChannel", {{"channelId", ch.id}, {"solo", !ch.solo}});
        ImGui::SameLine();
        if (toggleButton("Ø", ch.phaseInvert, col::Ivory, bs)) app.run("InvertPhase", {{"channelId", ch.id}, {"invert", !ch.phaseInvert}});
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}
} // namespace

void drawMixer(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    const float editorH = app.selSlot.empty() ? 0.0f : 240 * dpi;
    const float h = ImGui::GetContentRegionAvail().y - editorH - (editorH > 0 ? 8 : 0);
    const float w = 132 * dpi;
    if (ImGui::Button("+ Bus")) app.run("AddBus", {{"name", "Bus"}});
    ImGui::SameLine();
    if (!app.selChannel.empty()) {
        static int target = 0;
        std::vector<const MixerChannel*> busses;
        for (auto& c : p.channels)
            if (c.kind == ChannelKind::Bus && c.id != app.selChannel) busses.push_back(&c);
        if (!busses.empty()) {
            target = std::clamp(target, 0, static_cast<int>(busses.size()) - 1);
            ImGui::SetNextItemWidth(140 * dpi);
            if (ImGui::BeginCombo("##sendTarget", busses[static_cast<size_t>(target)]->name.c_str())) {
                for (size_t i = 0; i < busses.size(); ++i)
                    if (ImGui::Selectable(busses[i]->name.c_str(), static_cast<int>(i) == target)) target = static_cast<int>(i);
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Add send from selected"))
                app.run("AddSend", {{"channelId", app.selChannel}, {"target", busses[static_cast<size_t>(target)]->id}, {"levelDb", -12.0}});
        }
    }
    ImGui::BeginChild("strips", ImVec2(0, h - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float sh = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize - 4;
    for (auto& c : p.channels) {
        if (c.kind == ChannelKind::Master) continue;
        strip(app, c, w, sh, false);
        ImGui::SameLine();
    }
    ImGui::Dummy(ImVec2(12, 1));
    ImGui::SameLine();
    if (MixerChannel* m = p.master()) strip(app, *m, w * 1.2f, sh, true);
    ImGui::EndChild();
    if (editorH > 0) {
        MixerChannel* owner = nullptr;
        PluginSlot* slot = p.findSlot(app.selSlot, &owner);
        if (!slot) {
            app.selSlot.clear();
            return;
        }
        ImGui::BeginChild("editor", ImVec2(0, editorH), ImGuiChildFlags_Borders);
        sectionTitle(std::format("{}  -  {}", slot->name, owner ? owner->name : "").c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 60 * dpi);
        if (ImGui::SmallButton("close")) app.selSlot.clear();
        paramEditor(app, slot->id);
        ImGui::EndChild();
    }
}

void drawMaster(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginChild("masterLeft", ImVec2(420 * dpi, 0), ImGuiChildFlags_Borders);
    sectionTitle("MASTER CHAIN");
    ImGui::TextDisabled("Presets replace the master inserts (one undo step).");
    for (const char* preset : {"streaming", "dynamic", "club", "voice"}) {
        if (ImGui::Button(preset, ImVec2(92 * dpi, 0))) app.run("CreateMasterChain", {{"preset", preset}});
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (MixerChannel* m = p.master()) {
        for (auto& s : m->inserts) {
            ImGui::PushID(s.id.c_str());
            if (toggleButton(s.bypass ? "off" : "on", !s.bypass, col::Green, ImVec2(36 * dpi, 0))) app.run("BypassInsert", {{"slotId", s.id}, {"bypass", !s.bypass}});
            ImGui::SameLine();
            if (ImGui::Selectable(s.name.c_str(), app.selSlot == s.id)) app.selSlot = s.id;
            ImGui::PopID();
        }
        if (!app.selSlot.empty() && p.findSlot(app.selSlot)) {
            ImGui::Separator();
            paramEditor(app, app.selSlot);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("masterRight", ImVec2(0, 0), ImGuiChildFlags_Borders);
    sectionTitle("LOUDNESS & MIX CHECK");
    if (goldButton("Analyse mix")) {
        if (app.run("AnalyzeMix", {{"startBeat", 0.0}, {"endBeat", std::max(4.0, p.endBeat())}})) app.masterResult = app.lastResult();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("offline render of the whole song through the real engine");
    if (app.masterResult.is_object() && app.masterResult.contains("master")) {
        const json& m = app.masterResult["master"];
        auto num = [&](const char* k) { return m.contains(k) && m[k].is_number() ? m[k].get<double>() : 0.0; };
        ImGui::Text("Loudness %.1f LUFS   Peak %.1f dBFS   True peak %.1f dBTP   Correlation %.2f   Low-end side %.1f dB", num("lufs"), num("peakDb"),
                    num("truePeakDb"), num("correlation"), num("lowSideRatioDb"));
        int k = 0;
        for (auto& is : app.masterResult.value("issues", json::array())) {
            const std::string sev = is.value("severity", "info");
            ImGui::PushID(k++);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sev == "problem" || sev == "high" ? col::Red : sev == "warning" || sev == "medium" ? col::Orange : col::IvoryDim),
                               "%s", is.value("title", "").c_str());
            ImGui::TextDisabled("   %s", is.value("detail", "").c_str());
            int n = 0;
            for (auto& sg : is.value("suggestions", json::array())) {
                ImGui::PushID(n++);
                ImGui::TextDisabled("   -> %s", sg.value("description", "").c_str());
                if (sg.value("command", "") != "") {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("apply")) app.run(sg["command"].get<std::string>(), sg.value("args", json::object()));
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    sectionTitle("EXPORT");
    static int fmt = 0, bits = 1, norm = 0, stems = 0;
    static float lufs = -14.0f, ceiling = -1.0f;
    const char* fmts[] = {"WAV", "FLAC", "MP3 (not available)"};
    const char* bitsN[] = {"16 bit", "24 bit", "32 bit float"};
    const char* norms[] = {"none", "peak", "loudness"};
    const char* stemN[] = {"mixdown only", "all tracks", "busses", "vocals", "instrumental"};
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Format", &fmt, fmts, 3);
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Bit depth", &bits, bitsN, 3);
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Normalize", &norm, norms, 3);
    if (norm == 2) {
        ImGui::SetNextItemWidth(160 * dpi);
        ImGui::SliderFloat("Target LUFS", &lufs, -23, -6, "%.1f");
    }
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::SliderFloat("True-peak ceiling", &ceiling, -3, 0, "%.1f dBTP");
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Stems", &stems, stemN, 5);
    if (goldButton("EXPORT", ImVec2(160 * dpi, 0))) {
        static const char* stemIds[] = {"none", "tracks", "busses", "vocals", "instrumental"};
        const int bitVals[] = {16, 24, 32};
        if (app.run("Export", {{"format", fmt == 0 ? "wav" : fmt == 1 ? "flac" : "mp3"}, {"bitDepth", bitVals[bits]}, {"normalize", norms[norm]},
                               {"lufs", lufs}, {"ceilingDb", ceiling}, {"stems", stemIds[stems]}})) {
            for (auto& f : app.lastResult()["files"])
                app.message(0, std::format("exported {} ({:.1f} LUFS, {:.1f} dBTP)", f.value("path", ""), f.value("lufs", 0.0), f.value("truePeakDb", 0.0)));
        }
    }
    ImGui::TextDisabled("Files go to <project>/Exports. Existing files are never overwritten.");
    ImGui::EndChild();
}

} // namespace roy::gui
