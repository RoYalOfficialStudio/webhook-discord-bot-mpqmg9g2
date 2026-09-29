// MIXER (channel strips, inserts, sends, parameter editor) and MASTER (chain, loudness, export).
#include "Ui.h"

#include "export/Mp3Encoder.h"
#include "midi/MidiLearn.h"
#include "plugins/Sandbox.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::gui {

void midiLearnMenuItems(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId, const std::string& label) {
    const MidiMapping* m = midi::findMappingForTarget(app.project(), channelId, slotId, paramId);
    if (!app.midiInput()) {
        ImGui::MenuItem("MIDI Learn (no MIDI input)", nullptr, false, false);
        return;
    }
    if (ImGui::MenuItem(m ? "MIDI Learn (re-map)" : "MIDI Learn")) app.startMidiLearn(channelId, slotId, paramId, label);
    if (m && ImGui::MenuItem(std::format("Remove MIDI mapping (CC {}{})", m->cc, m->channel >= 0 ? std::format(", ch {}", m->channel + 1) : "").c_str()))
        app.run("RemoveMidiMapping", {{"mappingId", m->id}});
}

bool midiMappedTag(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId) {
    const MidiMapping* m = midi::findMappingForTarget(app.project(), channelId, slotId, paramId);
    if (!m) return false;
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "CC%d", m->cc);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("controlled by MIDI CC %d%s - right-click the control to re-map or remove", m->cc,
                                                  m->channel >= 0 ? std::format(" on channel {}", m->channel + 1).c_str() : "");
    return true;
}

void channelPresetMenu(App& app, const std::string& channelId, const std::string& suggestedName) {
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::TextDisabled("VOCAL CHAIN / CHANNEL PRESET");
    if (ImGui::BeginMenu("Load preset")) {
        ImGui::TextDisabled("replaces this channel's effects");
        ImGui::TextDisabled("(Ctrl+Z undoes, tune follows the song key)");
        ImGui::Separator();
        ImGui::TextDisabled("RoY starting points");
        for (auto& f : presets::factoryChannelPresets())
            if (ImGui::MenuItem(f.value("name", std::string("?")).c_str())) app.applyChannelPreset(channelId, f);
        ImGui::Separator();
        ImGui::TextDisabled("Your presets");
        const auto& list = app.channelPresets(ImGui::IsWindowAppearing());
        if (list.empty()) ImGui::TextDisabled("  (none saved yet)");
        for (auto& pf : list)
            if (ImGui::MenuItem(pf.name.c_str())) {
                std::string err;
                if (auto pr = presets::loadChannelPreset(pf.file, &err)) app.applyChannelPreset(channelId, *pr);
                else app.message(2, "preset not loaded: " + err);
            }
        ImGui::EndMenu();
    }
    static char pname[64];
    static std::string lastChannel, confirmName;
    if (ImGui::IsWindowAppearing() || lastChannel != channelId) {
        std::snprintf(pname, sizeof(pname), "%s", suggestedName.c_str());
        lastChannel = channelId;
        confirmName.clear();
    }
    ImGui::SetNextItemWidth(170 * dpi);
    ImGui::InputText("##presetname", pname, sizeof(pname));
    ImGui::SameLine();
    const std::string name(pname);
    if (ImGui::Button("Save preset") && !name.empty()) {
        bool exists = false;
        for (auto& pf : app.channelPresets(true)) exists |= presets::presetFileStem(pf.name) == presets::presetFileStem(name);
        if (exists) confirmName = name;
        else if (app.saveChannelPreset(channelId, name, false)) ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("saves effects + settings + fader/pan for the next song");
    if (!confirmName.empty() && confirmName == name) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "'%s' exists.", name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Replace (old version is kept in Backups)")) {
            if (app.saveChannelPreset(channelId, name, true)) ImGui::CloseCurrentPopup();
            confirmName.clear();
        }
    }
    if (ImGui::MenuItem("Open presets folder")) app.openFolder(presets::channelPresetDirectory());
}

void liveVocalButton(App& app, const std::string& trackId, const ImVec2& size) {
    Project& p = app.project();
    const Track* t = p.findTrack(trackId);
    if (!t) return;
    const MixerChannel* ch = p.findChannel(t->channelId);
    const PluginSlot* tune = nullptr;
    if (ch)
        for (auto& s : ch->inserts)
            if (s.typeId == "roy.vocaltune" && !tune) tune = &s;
    const bool live = t->monitor && tune && !tune->bypass;
    if (toggleButton("LIVE", live, col::Green, size)) {
        if (live) app.run("MonitorTrack", {{"trackId", trackId}, {"monitor", false}});
        else app.liveVocal(trackId);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", live ? "LIVE VOCAL is on: you hear yourself with autotune. Click = off.  Right-click: speed, strength, presets"
                                     : "LIVE VOCAL: hear your voice in real time with autotune (song key) while recording.\n"
                                       "Use headphones!  Right-click: speed, strength, save/load vocal chain");
    ImGui::PushID(trackId.c_str());
    if (ImGui::BeginPopupContextItem("liveMenu")) {
        const float dpi = ImGui::GetFontSize() / 15.0f;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "LIVE VOCAL - %s", t->name.c_str());
        bool mon = t->monitor;
        if (ImGui::Checkbox("hear my input (monitor)", &mon)) app.run("MonitorTrack", {{"trackId", trackId}, {"monitor", mon}});
        if (!tune) {
            if (ImGui::Button("Add autotune (RoY VocalTune)")) app.liveVocal(trackId);
        } else {
            const std::string slotId = tune->id;
            bool on = !tune->bypass;
            if (ImGui::Checkbox("autotune on", &on)) app.run("BypassInsert", {{"slotId", slotId}, {"bypass", !on}});
            ImGui::SameLine();
            ImGui::TextDisabled("key %s (song key)", p.key.name().c_str());
            if (auto proc = app.runtime().processorForSlot(slotId)) {
                auto slider = [&](const char* id, const char* label, float lo, float hi, const char* fmt, const char* tip) {
                    for (int i = 0; i < proc->numParams(); ++i)
                        if (proc->paramInfo(i).id == id) {
                            float v = proc->getParam(i);
                            ImGui::SetNextItemWidth(200 * dpi);
                            if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) proc->setParam(i, v); // audible while dragging
                            if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetParam", {{"slotId", slotId}, {"paramId", id}, {"value", proc->getParam(i)}});
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                        }
                };
                slider("speed", "retune speed", 0, 200, "%.0f ms", "0-10 ms = hard autotune effect (rap/trap), 40-100 ms = natural correction");
                slider("strength", "strength", 0, 1, "%.2f", "1 = fully in tune, lower = keep some of your own pitch");
            }
        }
        ImGui::Separator();
        if (ch) channelPresetMenu(app, ch->id, t->name + " chain");
        ImGui::Separator();
        if (ImGui::MenuItem("Mix it in the MIXER (EQ, compressor, reverb ...)")) {
            app.selChannel = t->channelId;
            app.area = Area::Mixer;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

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
            }
            if (ImGui::BeginPopupContextItem("pctx")) {
                if (ImGui::MenuItem("Default value")) {
                    proc->setParam(i, pi.defaultValue);
                    edited = true;
                }
                if (owner) midiLearnMenuItems(app, owner->id, slotId, pi.id, (slot ? slot->name + " · " : std::string()) + pi.name);
                ImGui::EndPopup();
            }
            if (owner) midiMappedTag(app, owner->id, slotId, pi.id);
            if (edited) app.run("SetParam", {{"slotId", slotId}, {"paramId", pi.id}, {"value", proc->getParam(i)}});
            ImGui::PopID();
        }
    ImGui::TextDisabled("right-click a control: default value, MIDI Learn | + / *: favourite");
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
    if (ImGui::BeginPopupContextItem("chPresetMenu")) {
        channelPresetMenu(app, ch.id, ch.name + " chain");
        ImGui::EndPopup();
    }
    if (!master) { // PRESETS: save this mix for the next song / load a saved one
        if (ImGui::Button("PRESETS", ImVec2(-1, 0))) ImGui::OpenPopup("chPresetMenu");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("save this channel's effects + settings as a preset, or load one (e.g. your vocal chain)");
    }
    // inserts + sends live in a scrollable rack; the bar below it drags the rack taller / shorter
    static float rackFrac = 0.45f; // share of the strip height above the pan/fader section (all strips)
    const float bottomH = height * (1.0f - rackFrac);
    const float splitH = 8 * dpi;
    const float rackH = std::max(40 * dpi, height - bottomH - ImGui::GetCursorPosY() - splitH - 4 * dpi);
    ImGui::BeginChild("rack", ImVec2(-1, rackH), ImGuiChildFlags_None);
    ImGui::TextDisabled("INSERTS (%zu)", ch.inserts.size());
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
        if (ImGui::Selectable(label.c_str(), app.selSlot == s.id, 0, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
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
    if (!ch.sends.empty()) ImGui::TextDisabled("SENDS");
    for (size_t si = 0; si < ch.sends.size(); ++si) {
        const Send& s = ch.sends[si];
        const MixerChannel* t = app.project().findChannel(s.targetChannelId);
        if (!t) continue;
        ImGui::PushID(s.id.c_str());
        float lvl = s.levelDb;
        ImGui::SetNextItemWidth(-1);
        const std::string fmt = t->name + " %.0f dB";
        if (ImGui::SliderFloat("##send", &lvl, -60.0f, 6.0f, fmt.c_str()) && params && si < ChannelParams::kMaxSends)
            params->sendLevelDb[si].store(lvl); // live while dragging
        if (editFinished(lvl)) app.run("SetSend", {{"sendId", s.id}, {"levelDb", lvl}});
        if (ImGui::BeginPopupContextItem("sendctx")) {
            if (ImGui::MenuItem(s.preFader ? "Switch to POST fader" : "Switch to PRE fader"))
                app.run("SetSend", {{"sendId", s.id}, {"preFader", !s.preFader}});
            if (ImGui::MenuItem(s.enabled ? "Disable send" : "Enable send")) app.run("SetSend", {{"sendId", s.id}, {"enabled", !s.enabled}});
            if (ImGui::MenuItem("Automate send level")) {
                const double b = app.positionBeats();
                app.run("CreateAutomation", {{"channelId", ch.id}, {"paramId", "send:" + s.id}, {"points", {{b, s.levelDb}, {b + 4.0, s.levelDb}}}});
            }
            if (ImGui::MenuItem("Remove send")) app.run("RemoveSend", {{"sendId", s.id}});
            ImGui::Separator();
            midiLearnMenuItems(app, ch.id, "", "send:" + s.id, ch.name + " · Send " + t->name);
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %s%s - right-click: pre/post, on/off, automate, remove, MIDI learn", s.preFader ? "PRE" : "POST", t->name.c_str(),
                              s.enabled ? "" : " (off)");
        ImGui::PopID();
    }
    if (!master && ch.sends.size() < ChannelParams::kMaxSends && ImGui::SmallButton("+ send")) ImGui::OpenPopup("addSend");
    if (ImGui::BeginPopup("addSend")) {
        bool any = false;
        for (auto& b : app.project().channels)
            if (b.kind == ChannelKind::Bus && b.id != ch.id) {
                any = true;
                if (ImGui::MenuItem(b.name.c_str())) app.run("AddSend", {{"channelId", ch.id}, {"target", b.id}, {"levelDb", -6.0}});
            }
        if (!any) ImGui::TextDisabled("no bus yet");
        if (ImGui::MenuItem("+ new FX bus")) {
            if (app.run("AddBus", {{"name", "FX"}})) {
                const std::string bus = app.lastResult().value("id", "");
                if (!bus.empty()) app.run("AddSend", {{"channelId", ch.id}, {"target", bus}, {"levelDb", -6.0}});
            }
        }
        ImGui::EndPopup();
    }
    const bool overflow = ImGui::GetScrollMaxY() > 0.0f;
    const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::EndChild();
    // splitter: drag up/down to show more effects or a longer fader (shared by all strips)
    {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("rackSplit", ImVec2(w, splitH));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActive()) rackFrac = std::clamp(rackFrac + ImGui::GetIO().MouseDelta.y / height, 0.2f, 0.72f);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("drag up / down: more room for effects or for the fader.  Mouse wheel over the effects scrolls them.");
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        const float cy = a.y + splitH * 0.5f;
        sdl->AddRectFilled(ImVec2(a.x, cy - 1.5f * dpi), ImVec2(a.x + w, cy + 1.5f * dpi), hot ? col::Gold : col::GoldDim, 2.0f);
        if (overflow && !atEnd) // more effects below: small arrow hint
            sdl->AddTriangleFilled(ImVec2(a.x + w * 0.5f - 5 * dpi, cy - 4 * dpi), ImVec2(a.x + w * 0.5f + 5 * dpi, cy - 4 * dpi),
                                   ImVec2(a.x + w * 0.5f, cy + 3 * dpi), col::Orange);
    }
    // pan
    ImGui::SetCursorPosY(height - bottomH);
    float pan = ch.pan;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##pan", &pan, -1, 1, pan == 0 ? "C" : pan < 0 ? "L %.2f" : "R %.2f") && params) params->pan.store(pan);
    if (editFinished(pan)) app.run("SetChannelPan", {{"channelId", ch.id}, {"master", master}, {"pan", pan}});
    if (ImGui::BeginPopupContextItem("panctx")) {
        if (ImGui::MenuItem("Center")) app.run("SetChannelPan", {{"channelId", ch.id}, {"master", master}, {"pan", 0.0}});
        midiLearnMenuItems(app, ch.id, "", "pan", ch.name + " · Pan");
        ImGui::EndPopup();
    }
    // fader + meter
    float gain = ch.gainDb;
    const float faderH = bottomH - ImGui::GetFrameHeight() * (master ? 3.2f : 4.4f); // room for solo safe / delete bus
    ImGui::VSliderFloat("##fader", ImVec2(width * 0.42f, faderH), &gain, -60.0f, 12.0f, "%.1f dB");
    if (ImGui::IsItemActive() && params) params->gainDb.store(gain);
    if (editFinished(gain)) app.run("SetChannelGain", {{"channelId", ch.id}, {"master", master}, {"gainDb", gain}});
    if (ImGui::BeginPopupContextItem("faderctx")) {
        if (ImGui::MenuItem("Reset to 0 dB")) app.run("SetChannelGain", {{"channelId", ch.id}, {"master", master}, {"gainDb", 0.0}});
        midiLearnMenuItems(app, ch.id, "", "gain", ch.name + " · Volume");
        ImGui::EndPopup();
    }
    if (const MidiMapping* mm = midi::findMappingForTarget(app.project(), ch.id, "", "gain"); mm && ImGui::IsItemHovered())
        ImGui::SetTooltip("volume controlled by MIDI CC %d - right-click: reset, MIDI learn", mm->cc);
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
        if (ch.kind == ChannelKind::Track && toggleButton(ch.soloSafe ? "SOLO SAFE" : "solo safe", ch.soloSafe, col::Gold, ImVec2(-1, 0)))
            app.run("SetSoloSafe", {{"channelId", ch.id}, {"safe", !ch.soloSafe}});
        if (ch.kind == ChannelKind::Bus && ImGui::SmallButton("delete bus")) app.run("DeleteBus", {{"channelId", ch.id}});
    }
    ImGui::EndChild();
    // drop targets: plugin -> insert slot, audio file -> this channel's track at the playhead
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ROY_PLUGIN")) {
            const std::string s(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
            const auto nl = s.find('\n');
            const std::string typeId = s.substr(0, nl), name = nl == std::string::npos ? typeId : s.substr(nl + 1);
            if (app.run("AddInsert", {{"channelId", ch.id}, {"typeId", typeId}, {"name", name}})) {
                app.pluginDb().markUsed(typeId);
                app.savePluginDb();
            }
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ROY_FILE")) {
            const std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
            for (auto& t : app.project().tracks)
                if (t.channelId == ch.id && t.type == TrackType::Audio) {
                    app.run("ImportAudio", {{"path", path}, {"trackId", t.id}, {"startBeat", app.positionBeats()}});
                    break;
                }
        }
        ImGui::EndDragDropTarget();
    }
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
    static int fmt = 0, bits = 1, norm = 0, stems = 0, mp3Rate = 3, mp3Mode = 0, vbrQ = 2;
    static float lufs = -14.0f, ceiling = -1.0f;
    static char mArtist[128] = "", mAlbum[128] = "", mYear[16] = "", mTrack[16] = "", mComment[256] = "", mGenre[64] = "";
    const char* fmts[] = {"WAV", "FLAC", "MP3"};
    const char* bitsN[] = {"16 bit", "24 bit", "32 bit float"};
    const char* norms[] = {"none", "peak", "loudness"};
    const char* stemN[] = {"mixdown only", "all tracks", "busses", "vocals", "instrumental"};
    const char* rates[] = {"128 kbps", "192 kbps", "256 kbps", "320 kbps"};
    const int rateVals[] = {128, 192, 256, 320};
    const char* modes[] = {"CBR", "VBR"};
    ImGui::SetNextItemWidth(160 * dpi);
    ImGui::Combo("Format", &fmt, fmts, 3);
    if (fmt == 2) {
        static std::string mp3Status;
        static bool checked = false;
        if (!checked) {
            std::string why;
            mp3Status = mp3::available(&why) ? mp3::encoderVersion() : "NOT AVAILABLE: " + why;
            checked = true;
        }
        ImGui::TextDisabled("Encoder: %s (LGPL, separate library)", mp3Status.c_str());
        ImGui::SetNextItemWidth(160 * dpi);
        ImGui::Combo("Mode", &mp3Mode, modes, 2);
        ImGui::SetNextItemWidth(160 * dpi);
        if (mp3Mode == 0) ImGui::Combo("Bitrate", &mp3Rate, rates, 4);
        else ImGui::SliderInt("VBR quality (0 best)", &vbrQ, 0, 9);
        if (ImGui::TreeNode("Metadata")) {
            ImGui::InputText("Artist", mArtist, sizeof(mArtist));
            ImGui::InputText("Album", mAlbum, sizeof(mAlbum));
            ImGui::InputText("Track", mTrack, sizeof(mTrack));
            ImGui::InputText("Year", mYear, sizeof(mYear));
            ImGui::InputText("Genre", mGenre, sizeof(mGenre));
            ImGui::InputText("Comment", mComment, sizeof(mComment));
            ImGui::TextDisabled("Title = project name");
            ImGui::TreePop();
        }
    } else {
        ImGui::SetNextItemWidth(160 * dpi);
        ImGui::Combo("Bit depth", &bits, bitsN, fmt == 1 ? 2 : 3);
    }
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
        json args = {{"format", fmt == 0 ? "wav" : fmt == 1 ? "flac" : "mp3"}, {"bitDepth", bitVals[bits]}, {"normalize", norms[norm]},
                     {"lufs", lufs}, {"ceilingDb", ceiling}, {"stems", stemIds[stems]}};
        if (fmt == 2) {
            args["bitrate"] = rateVals[mp3Rate];
            args["mp3Mode"] = mp3Mode == 0 ? "cbr" : "vbr";
            args["vbrQuality"] = vbrQ;
            args["metadata"] = {{"title", p.name}, {"artist", std::string(mArtist)}, {"album", std::string(mAlbum)}, {"track", std::string(mTrack)},
                                {"year", std::string(mYear)}, {"genre", std::string(mGenre)}, {"comment", std::string(mComment)}};
        }
        if (app.run("Export", args)) {
            for (auto& f : app.lastResult()["files"])
                app.message(0, std::format("exported {} ({:.1f} LUFS, {:.1f} dBTP)", f.value("path", ""), f.value("lufs", 0.0), f.value("truePeakDb", 0.0)));
            for (auto& w : app.lastResult().value("warnings", json::array())) app.message(1, w.get<std::string>());
        }
    }
    ImGui::TextDisabled("Files go to <project>/Exports. Existing files are never overwritten.");
    ImGui::EndChild();
}

} // namespace roy::gui
