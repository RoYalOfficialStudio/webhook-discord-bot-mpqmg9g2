// PIANO ROLL, CHANNELS (step sequencer), BEATS (patterns, 808).
#include "Ui.h"
#include "beat/StepSequencer.h"

#include "midi/Scale.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::gui {

namespace {
const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

// How many playlist clips play this pattern (shown before deleting).
int patternUses(const Project& p, const std::string& id) {
    int n = 0;
    for (auto& t : p.tracks)
        for (auto& c : t.patternClips) n += c.patternId == id;
    return n;
}

// Deletes a pattern (and its playlist clips) and moves the selection to a neighbour.
void deletePattern(App& app, const std::string& id) {
    Project& p = app.project();
    std::string next;
    for (size_t i = 0; i < p.patterns.size(); ++i)
        if (p.patterns[i].id == id) {
            if (i + 1 < p.patterns.size()) next = p.patterns[i + 1].id;
            else if (i > 0) next = p.patterns[i - 1].id;
        }
    if (app.run("DeletePattern", {{"patternId", id}})) {
        const int clips = app.lastResult().value("removedClips", 0);
        app.message(0, clips ? std::format("pattern deleted with {} playlist clip(s) - Ctrl+Z brings it back", clips)
                             : std::string("pattern deleted - Ctrl+Z brings it back"));
        if (app.selPattern == id) app.selPattern = next;
    }
}

// Right-click menu of a pattern entry: rename, duplicate, delete. Returns the id to delete
// (deleting is deferred by the caller so the pattern list is not changed while iterating).
std::string patternMenu(App& app, const Pattern& pat) {
    std::string del;
    if (!ImGui::BeginPopupContextItem("patmenu")) return del;
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::TextDisabled("%s", pat.name.c_str());
    static char pname[96];
    if (ImGui::IsWindowAppearing()) std::snprintf(pname, sizeof(pname), "%s", pat.name.c_str());
    ImGui::SetNextItemWidth(160 * dpi);
    const bool enter = ImGui::InputText("##patname", pname, sizeof(pname), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Rename") || enter) {
        app.run("RenamePattern", {{"patternId", pat.id}, {"name", std::string(pname)}});
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Duplicate") && app.run("MakeVariation", {{"patternId", pat.id}, {"seed", 1}, {"amount", 0.0}, {"name", pat.name + " copy"}}))
        app.selPattern = app.lastResult().value("id", app.selPattern);
    ImGui::Separator();
    const int uses = patternUses(app.project(), pat.id);
    const std::string label = uses ? std::format("Delete pattern (+ {} playlist clip{})", uses, uses == 1 ? "" : "s") : std::string("Delete pattern");
    if (ImGui::MenuItem(label.c_str(), "Del")) del = pat.id;
    ImGui::EndPopup();
    return del;
}
} // namespace

// ---------------------------------------------------------------- PIANO ROLL
void drawPianoRoll(App& app) {
    Project& p = app.project();
    Track* owner = nullptr;
    MidiClip* clip = p.findMidiClip(app.selMidiClip, &owner);
    if (!clip) {
        // pick the first MIDI clip of the selected track
        if (Track* t = p.findTrack(app.selTrack); t && !t->midiClips.empty()) app.selMidiClip = t->midiClips[0].id;
        else
            for (auto& t : p.tracks)
                if (!t.midiClips.empty()) {
                    app.selMidiClip = t.midiClips[0].id;
                    break;
                }
        clip = p.findMidiClip(app.selMidiClip, &owner);
    }
    if (!clip) {
        ImGui::TextDisabled("No MIDI clip yet. Double-click a MIDI track lane in the PLAYLIST to create one.");
        return;
    }
    const float dpi = ImGui::GetFontSize() / 15.0f;
    static double gridBeats = 0.25;
    static float noteH = 14.0f;
    static std::vector<size_t> selection;

    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "%s", clip->name.empty() ? "MIDI clip" : clip->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("on %s | key %s | %zu notes", owner ? owner->name.c_str() : "?", p.key.name().c_str(), clip->notes.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * dpi);
    const char* grids[] = {"1/32", "1/16", "1/8", "1/4"};
    const double gv[] = {0.125, 0.25, 0.5, 1.0};
    int gi = 1;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(gridBeats - gv[i]) < 1e-9) gi = i;
    if (ImGui::Combo("Grid", &gi, grids, 4)) gridBeats = gv[gi];
    ImGui::SameLine();
    const char* modes[] = {"off", "highlight", "snap", "block"};
    int wm = static_cast<int>(p.settings.wrongNoteMode);
    ImGui::SetNextItemWidth(100 * dpi);
    if (ImGui::Combo("Wrong Note Blocker", &wm, modes, 4)) app.run("SetWrongNoteMode", {{"mode", modes[wm]}});
    ImGui::SameLine();
    if (ImGui::Button("Quantize")) app.run("QuantizeNotes", {{"clipId", clip->id}, {"grid", gridBeats}, {"strength", 1.0}});
    ImGui::SameLine();
    if (ImGui::Button("Humanize")) app.run("HumanizeNotes", {{"clipId", clip->id}});
    ImGui::SameLine();
    if (ImGui::Button("Snap to key")) app.run("SnapNotesToKey", {{"clipId", clip->id}});
    ImGui::SameLine();
    if (ImGui::Button("-12")) app.run("TransposeNotes", {{"clipId", clip->id}, {"amount", -12}});
    ImGui::SameLine();
    if (ImGui::Button("+12")) app.run("TransposeNotes", {{"clipId", clip->id}, {"amount", 12}});
    ImGui::SameLine();
    ImGui::Checkbox("Preview", &app.previewOnEdit);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hear a note when you click it, add it or click a piano key on the left");
    if (!selection.empty()) {
        ImGui::SameLine();
        json idx = json::array();
        for (auto i : selection) idx.push_back(i);
        if (ImGui::Button("Delete selected")) {
            app.run("DeleteNotes", {{"clipId", clip->id}, {"notes", idx}});
            selection.clear();
        }
        ImGui::SameLine();
        static int vel = 100;
        ImGui::SetNextItemWidth(120 * dpi);
        ImGui::SliderInt("Velocity", &vel, 1, 127);
        if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetNoteVelocity", {{"clipId", clip->id}, {"notes", idx}, {"velocity", vel}});
    }

    const float keysW = 56 * dpi;
    const double ppb = std::max(40.0, app.pixelsPerBeat * 2.0);
    ImGui::BeginChild("roll", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float h = noteH * dpi;
    const float W = keysW + static_cast<float>(clip->lengthBeats * ppb) + 40;
    const float H = h * 128;
    ImGui::Dummy(ImVec2(W, H));
    if (ImGui::IsWindowAppearing()) {
        int centre = 60;
        if (!clip->notes.empty()) {
            int lo = 127, hi = 0;
            for (auto& n : clip->notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
            centre = (lo + hi) / 2;
        }
        ImGui::SetScrollY(std::max(0.0f, (127 - centre) * h - ImGui::GetWindowHeight() * 0.5f));
    }
    const float sx = ImGui::GetScrollX();
    const float x0 = o.x + keysW;
    auto pitchY = [&](int pitch) { return o.y + (127 - pitch) * h; };
    // rows
    for (int pitch = 0; pitch < 128; ++pitch) {
        const float y = pitchY(pitch);
        const int pc = pitch % 12;
        const bool inKey = p.key.contains(pitch);
        ImU32 bg = isBlack(pc) ? col::rgb(0x101014) : col::rgb(0x17171C);
        if (inKey) bg = isBlack(pc) ? col::rgb(0x1A1810) : col::rgb(0x221F14);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(o.x + W, y + h), bg);
        if (pc == 0) dl->AddLine(ImVec2(x0, y + h), ImVec2(o.x + W, y + h), col::GridBar);
    }
    for (double b = 0; b <= clip->lengthBeats + 1e-9; b += gridBeats) {
        const bool beat = std::fabs(b - std::round(b)) < 1e-9;
        const bool bar = beat && static_cast<int>(std::round(b)) % 4 == 0;
        const float x = x0 + static_cast<float>(b * ppb);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + H), bar ? col::GoldDim : beat ? col::GridBar : col::Grid);
    }
    // ghost notes from other clips are drawn dim (context)
    // notes
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hover = -1;
    for (size_t i = 0; i < clip->notes.size(); ++i) {
        const MidiNote& n = clip->notes[i];
        const float nx0 = x0 + static_cast<float>(n.startBeat * ppb), nx1 = x0 + static_cast<float>((n.startBeat + n.lengthBeats) * ppb);
        const float ny = pitchY(n.pitch);
        const bool sel = std::find(selection.begin(), selection.end(), i) != selection.end();
        const bool outOfKey = !p.key.contains(n.pitch) && p.settings.wrongNoteMode != WrongNoteMode::Off;
        const float v = n.velocity / 127.0f;
        ImU32 c = outOfKey ? col::Red : col::rgb(0xFF8C00);
        ImVec4 cf = ImGui::ColorConvertU32ToFloat4(c);
        cf.w = 0.45f + 0.55f * v;
        dl->AddRectFilled(ImVec2(nx0 + 1, ny + 1), ImVec2(std::max(nx0 + 4, nx1 - 1), ny + h - 1), ImGui::ColorConvertFloat4ToU32(cf), 3);
        if (sel) dl->AddRect(ImVec2(nx0 + 1, ny + 1), ImVec2(std::max(nx0 + 4, nx1 - 1), ny + h - 1), col::Ivory, 3.0f, 0, 2.0f);
        if (n.slide) dl->AddText(ImVec2(nx0 + 3, ny), col::Obsidian, "~");
        if (mouse.x >= nx0 && mouse.x < nx1 && mouse.y >= ny && mouse.y < ny + h) hover = static_cast<int>(i);
    }
    // piano keys: click = hear the note (submitted before the canvas so the sticky keys win the hover)
    const std::string ownerId = owner ? owner->id : std::string();
    auto audition = [&](int pitch) {
        if (app.previewOnEdit && !ownerId.empty() && pitch >= 0 && pitch < 128) app.auditionNote(ownerId, pitch, 0.85f, 0.6);
    };
    ImGui::SetCursorScreenPos(ImVec2(o.x + ImGui::GetScrollX(), o.y));
    ImGui::InvisibleButton("keys", ImVec2(keysW, H));
    if (ImGui::IsItemActivated()) audition(127 - static_cast<int>((mouse.y - o.y) / h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("click: hear this note");
    // canvas interaction
    ImGui::SetCursorScreenPos(ImVec2(x0, o.y));
    ImGui::InvisibleButton("canvas", ImVec2(std::max(1.0f, W - keysW), H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const int mp = 127 - static_cast<int>((mouse.y - o.y) / h);
    const double mb = std::floor(((mouse.x - x0) / ppb) / gridBeats) * gridBeats;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hover >= 0) {
            if (!ImGui::GetIO().KeyShift) selection.clear();
            selection.push_back(static_cast<size_t>(hover));
            audition(clip->notes[static_cast<size_t>(hover)].pitch);
        } else if (mp >= 0 && mp < 128 && mb >= 0 && mb < clip->lengthBeats) {
            selection.clear();
            if (app.run("AddNote", {{"clipId", clip->id}, {"pitch", mp}, {"startBeat", mb}, {"lengthBeats", gridBeats * (gridBeats < 0.5 ? 2 : 1)}, {"velocity", 100}}))
                audition(mp);
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hover >= 0) {
        app.run("DeleteNotes", {{"clipId", clip->id}, {"notes", json::array({hover})}});
        selection.clear();
    }
    if (ImGui::IsItemHovered() && mp >= 0 && mp < 128) {
        ImGui::SetTooltip("%s%d  beat %.2f%s", kNoteNames[mp % 12], mp / 12 - 1, mb + 1, p.key.contains(mp) ? "" : "  (not in key)");
    }
    // piano keys (sticky)
    const float kx = o.x + sx;
    for (int pitch = 0; pitch < 128; ++pitch) {
        const float y = pitchY(pitch);
        const int pc = pitch % 12;
        dl->AddRectFilled(ImVec2(kx, y), ImVec2(kx + keysW, y + h - 1), isBlack(pc) ? col::rgb(0x1A1A1E) : col::rgb(0xE8E4D6));
        if (p.key.contains(pitch)) dl->AddRectFilled(ImVec2(kx + keysW - 5, y), ImVec2(kx + keysW, y + h - 1), col::Gold);
        if (pc == 0) dl->AddText(ImVec2(kx + 4, y), col::Obsidian, std::format("C{}", pitch / 12 - 1).c_str());
    }
    // playhead inside clip
    const double rel = app.positionBeats() - clip->startBeat;
    if (rel >= 0 && rel <= clip->lengthBeats) dl->AddLine(ImVec2(x0 + static_cast<float>(rel * ppb), o.y), ImVec2(x0 + static_cast<float>(rel * ppb), o.y + H), col::Gold, 2);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- CHANNELS (step sequencer)
void drawChannels(App& app) {
    Project& p = app.project();
    if (p.patterns.empty()) {
        ImGui::TextDisabled("No pattern yet.");
        if (goldButton("New Pattern")) app.run("AddPattern", {{"name", "Pattern 1"}});
        return;
    }
    Pattern* pat = p.findPattern(app.selPattern);
    if (!pat) {
        pat = &p.patterns.front();
        app.selPattern = pat->id;
    }
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextItemWidth(200 * dpi);
    if (ImGui::BeginCombo("Pattern", pat->name.c_str())) {
        for (auto& q : p.patterns)
            if (ImGui::Selectable(q.name.c_str(), q.id == pat->id)) app.selPattern = q.id;
        ImGui::EndCombo();
    }
    if (const std::string d = patternMenu(app, *pat); !d.empty()) {
        deletePattern(app, d);
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        app.run("AddPattern", {{"name", std::format("Pattern {}", p.patterns.size() + 1)}, {"steps", pat->numSteps}});
        app.selPattern = app.lastResult().value("id", app.selPattern);
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        deletePattern(app, pat->id);
        return;
    }
    if (ImGui::IsItemHovered()) {
        const int uses = patternUses(p, pat->id);
        ImGui::SetTooltip("%s", uses ? std::format("Delete '{}' + its {} playlist clip(s) (Ctrl+Z undoes)", pat->name, uses).c_str()
                                     : std::format("Delete '{}' (Ctrl+Z undoes)", pat->name).c_str());
    }
    ImGui::SameLine();
    float swing = pat->swing;
    ImGui::SetNextItemWidth(140 * dpi);
    ImGui::SliderFloat("Swing", &swing, 0, 1, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetPatternSwing", {{"patternId", pat->id}, {"swing", swing}});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * dpi);
    if (ImGui::BeginCombo("Groove", pat->groove.c_str())) {
        for (auto& g : beat::grooveTemplates()) {
            if (ImGui::Selectable(g.name.c_str(), g.name == pat->groove)) app.run("SetPatternGroove", {{"patternId", pat->id}, {"groove", g.name}});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.description.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    float gAmt = pat->grooveAmount;
    ImGui::SetNextItemWidth(90 * dpi);
    ImGui::SliderFloat("##gamt", &gAmt, 0, 1, "groove %.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetPatternGroove", {{"patternId", pat->id}, {"amount", gAmt}});
    ImGui::SameLine();
    std::string beatTrack;
    for (auto& t : p.tracks)
        if (t.type == TrackType::Beat && (beatTrack.empty() || t.id == app.selTrack)) beatTrack = t.id;
    if (ImGui::Button("Place in Playlist")) {
        if (beatTrack.empty()) {
            app.run("AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"role", "drums"}});
            beatTrack = app.lastResult().value("id", "");
        }
        double end = 0;
        if (const Track* t = p.findTrack(beatTrack))
            for (auto& c : t->patternClips) end = std::max(end, c.endBeat());
        app.run("AddPatternClip", {{"trackId", beatTrack}, {"patternId", pat->id}, {"startBeat", end}});
    }
    ImGui::Spacing();
    const float nameW = 120 * dpi, stepW = 26 * dpi, stepH = 30 * dpi;
    ImGui::BeginChild("steps", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const std::string pid = pat->id;
    for (size_t r = 0; r < pat->rows.size(); ++r) {
        PatternRow& row = pat->rows[r];
        ImGui::PushID(static_cast<int>(r));
        if (toggleButton("M", row.muted, col::Orange, ImVec2(22 * dpi, stepH)))
            app.run("SetRowMix", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"muted", !row.muted}});
        ImGui::SameLine();
        ImGui::Button(row.sampleAssetId.empty() ? row.name.c_str() : ("~ " + row.name).c_str(), ImVec2(nameW, stepH));
        if (ImGui::BeginDragDropTarget()) { // Browser -> Drum Pad
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ROY_FILE")) {
                const fs::path file(std::string(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize)));
                const std::string asset = app.importAsset(file);
                if (!asset.empty()) app.run("SetRowSample", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"assetId", asset}, {"name", file.stem().string()}});
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("rowctx")) {
            if (ImGui::BeginMenu("Velocity curve")) {
                for (auto& c : beat::velocityCurves())
                    if (ImGui::MenuItem(c.c_str())) app.run("SetVelocityCurve", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"curve", c}, {"lo", 0.4}, {"hi", 1.0}});
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Note repeat (whole row)")) {
                for (auto& rate : beat::noteRepeatRates())
                    if (ImGui::MenuItem(rate.c_str())) app.run("NoteRepeat", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"rate", rate}, {"velocity", 0.75}});
                ImGui::EndMenu();
            }
            if (!row.sampleAssetId.empty() && ImGui::MenuItem("Back to built-in drum voice"))
                app.run("SetRowSample", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"assetId", ""}});
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        float vol = row.volume;
        ImGui::SetNextItemWidth(60 * dpi);
        ImGui::SliderFloat("##vol", &vol, 0, 1, "");
        if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetRowMix", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"volume", vol}});
        for (size_t s = 0; s < row.steps.size(); ++s) {
            ImGui::SameLine(0, s % 4 == 0 ? 8 * dpi : 2 * dpi);
            const Step& st = row.steps[s];
            const bool beatGroup = (s / 4) % 2 == 0;
            ImU32 on = st.velocity < 0.6f ? col::GoldDim : col::Gold;
            ImGui::PushID(static_cast<int>(s));
            if (!st.on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(beatGroup ? col::rgb(0x26262E) : col::rgb(0x1C1C22)));
            const bool clicked = toggleButton("##s", st.on, on, ImVec2(stepW, stepH));
            if (!st.on) ImGui::PopStyleColor();
            if (clicked) app.run("SetStep", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"step", static_cast<int>(s)}, {"on", !st.on}});
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && st.on && !ImGui::GetIO().KeyShift)
                app.run("SetStep", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"step", static_cast<int>(s)}, {"velocity", st.velocity < 0.6f ? 1.0 : 0.45}});
            if (st.on && ImGui::GetIO().KeyShift && ImGui::BeginPopupContextItem("stepctx")) { // Shift+right-click: step details
                const json key = {{"patternId", pid}, {"row", static_cast<int>(r)}, {"step", static_cast<int>(s)}, {"on", true}};
                auto set = [&](const char* k, const json& v) { json a = key; a[k] = v; app.run("SetStep", a); };
                float prob = st.probability;
                if (ImGui::SliderFloat("probability", &prob, 0, 1, "%.2f")) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) set("probability", prob);
                int ratchet = st.roll;
                if (ImGui::SliderInt("ratchet", &ratchet, 0, 8)) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) set("roll", ratchet);
                if (ImGui::MenuItem("flam", nullptr, st.flam)) set("flam", !st.flam);
                float micro = st.microTiming;
                if (ImGui::SliderFloat("micro timing", &micro, -0.5f, 0.5f, "%.2f step")) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) set("microTiming", micro);
                ImGui::EndPopup();
            }
            if (st.on && (st.roll > 1 || st.flam || st.probability < 1.0f)) {
                const ImVec2 mn = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 2, mn.y), col::rgb(0x101014),
                                                    st.roll > 1 ? std::format("{}", st.roll).c_str() : st.flam ? "f" : "?");
            }
            ImGui::PopID();
            if (static_cast<int>(r) < 0) break;
        }
        ImGui::PopID();
        if (app.project().findPattern(pid) != pat) break; // pattern vector changed (undo etc.)
    }
    ImGui::TextDisabled("Left-click: step on/off  |  right-click: accent/ghost  |  Shift+right-click: probability / ratchet / flam  |  right-click row name: velocity curve, note repeat");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- BEATS (patterns, 808 lab)
void drawBeats(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginChild("patterns", ImVec2(320 * dpi, 0), ImGuiChildFlags_Borders);
    sectionTitle("PATTERNS");
    std::string toDelete;
    bool listFocused = false;
    for (auto& pat : p.patterns) {
        int steps = 0;
        for (auto& r : pat.rows)
            for (auto& s : r.steps) steps += s.on;
        ImGui::PushID(pat.id.c_str());
        const float xW = ImGui::GetFrameHeight();
        if (ImGui::Selectable(std::format("{}   ({} steps, {} hits)", pat.name, pat.numSteps, steps).c_str(), pat.id == app.selPattern,
                              ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - xW - 4 * dpi, 0)))
            app.selPattern = pat.id;
        if (ImGui::IsItemFocused()) listFocused = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("click: select  |  right-click: rename / duplicate / delete  |  Del: delete");
        if (const std::string d = patternMenu(app, pat); !d.empty()) toDelete = d;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::rgb(0x3A1C1C)));
        if (ImGui::Button("x", ImVec2(xW, 0))) toDelete = pat.id;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const int uses = patternUses(p, pat.id);
            ImGui::SetTooltip("%s", uses ? std::format("Delete pattern + its {} playlist clip(s) (Ctrl+Z undoes)", uses).c_str()
                                         : "Delete pattern (Ctrl+Z undoes)");
        }
        ImGui::PopID();
    }
    // Del / Backspace deletes the selected pattern while the pattern list has focus or the mouse is over it.
    if (!app.selPattern.empty() && !ImGui::GetIO().WantTextInput &&
        (listFocused || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        toDelete = app.selPattern;
    if (!toDelete.empty()) deletePattern(app, toDelete);
    if (goldButton("New 16")) app.run("AddPattern", {{"steps", 16}});
    ImGui::SameLine();
    if (ImGui::Button("New 32")) app.run("AddPattern", {{"steps", 32}});
    ImGui::SameLine();
    if (ImGui::Button("Edit in CHANNELS")) app.area = Area::Channels;
    ImGui::Spacing();
    sectionTitle("QUICK GROOVES");
    struct Groove { const char* name; const char* kick; const char* snare; const char* hat; };
    static const Groove grooves[] = {{"Trap", "X.....x...x.....", "....X.......X...", "xoxoxoxoxoxoxxxx"},
                                     {"Boom Bap", "X......x..X.....", "....X.......X...", "x.x.x.x.x.x.x.x."},
                                     {"Drill", "X......x.x......", "......X.......X.", "x..x..x.x..x..x."},
                                     {"Four on the floor", "X...X...X...X...", "....X.......X...", "..x...x...x...x."}};
    Pattern* sel = p.findPattern(app.selPattern);
    ImGui::PushID("quickgrooves"); // same labels as GENERATE ("Trap", "Boom Bap", ...) -> own ID scope
    for (auto& g : grooves) {
        if (ImGui::Button(g.name) && sel) // one button = one undo step
            app.runMacro(std::string("Quick Groove: ") + g.name,
                         {{"SetRowPattern", {{"patternId", sel->id}, {"voice", "kick"}, {"text", g.kick}}},
                          {"SetRowPattern", {{"patternId", sel->id}, {"voice", "snare"}, {"text", g.snare}}},
                          {"SetRowPattern", {{"patternId", sel->id}, {"voice", "closed_hat"}, {"text", g.hat}}}});
        ImGui::SameLine();
    }
    ImGui::PopID();
    ImGui::NewLine();
    if (!sel) ImGui::TextDisabled("select a pattern above first");
    ImGui::Spacing();
    sectionTitle("GENERATE");
    ImGui::PushID("generate");
    static int seed = 1;
    ImGui::SetNextItemWidth(90 * dpi);
    ImGui::InputInt("seed", &seed);
    for (auto& style : beat::generatorStyles()) {
        if (ImGui::Button(style.c_str()) && app.run("GeneratePattern", {{"style", style}, {"seed", seed}}))
            app.selPattern = app.lastResult().value("id", app.selPattern);
        ImGui::SameLine();
    }
    ImGui::PopID();
    ImGui::NewLine();
    if (sel && ImGui::Button("Make Variation") && app.run("MakeVariation", {{"patternId", sel->id}, {"seed", seed++}, {"amount", 0.35}}))
        app.selPattern = app.lastResult().value("id", app.selPattern);
    ImGui::Spacing();
    sectionTitle("PATTERN CHAIN");
    static std::vector<std::string> chain;
    std::erase_if(chain, [&](const std::string& id) { return !p.findPattern(id); });
    std::string chainText;
    for (auto& id : chain) chainText += (chainText.empty() ? "" : " > ") + p.findPattern(id)->name;
    ImGui::TextWrapped("%s", chain.empty() ? "(empty - add patterns in playing order)" : chainText.c_str());
    if (sel && ImGui::SmallButton("+ selected")) chain.push_back(sel->id);
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) chain.clear();
    static int repeats = 1;
    ImGui::SetNextItemWidth(90 * dpi);
    ImGui::InputInt("repeats", &repeats);
    repeats = std::clamp(repeats, 1, 64);
    if (!chain.empty() && goldButton("Place chain")) {
        std::string beatTrack;
        for (auto& t : p.tracks)
            if (t.type == TrackType::Beat && (beatTrack.empty() || t.id == app.selTrack)) beatTrack = t.id;
        if (beatTrack.empty() && app.run("AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"role", "drums"}}))
            beatTrack = app.lastResult().value("id", "");
        double end = 0;
        if (const Track* t = p.findTrack(beatTrack))
            for (auto& c : t->patternClips) end = std::max(end, c.endBeat());
        if (!beatTrack.empty()) app.run("PlacePatternChain", {{"trackId", beatTrack}, {"patternIds", chain}, {"startBeat", end}, {"repeats", repeats}});
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("808lab", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::BeginChild("808params", ImVec2(340 * dpi, 0));
    sectionTitle("808 LAB");
    static int previewNote = 36; // C2 - a typical 808 root
    int n808 = 0;
    std::string previewTrack; // after a parameter change: hear the 808 (deferred, the track list must not change mid-loop)
    for (auto& t : p.tracks) {
        if (!t.instrument || t.instrument->typeId != "roy.808") continue;
        ++n808;
        ImGui::PushID(t.id.c_str());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "%s", t.name.c_str());
        // PREVIEW: hear the 808 without playing the song
        if (goldButton(std::format("PLAY 808  {}{}", kNoteNames[previewNote % 12], previewNote / 12 - 1).c_str(), ImVec2(150 * dpi, 0)))
            app.auditionNote(t.id, previewNote, 1.0f, 1.0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hear this 808 (works with the song stopped)");
        ImGui::SameLine();
        if (ImGui::Button("oct -") && previewNote >= 24) previewNote -= 12;
        ImGui::SameLine(0, 2);
        if (ImGui::Button("oct +") && previewNote < 72) previewNote += 12;
        // one octave of keys: click = choose the note and hear it
        const float keyW = std::floor((std::min(ImGui::GetContentRegionAvail().x, 330 * dpi) - 11 * 2) / 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, ImGui::GetStyle().FramePadding.y));
        for (int k = 0; k < 12; ++k) {
            const int note = (previewNote / 12) * 12 + k;
            ImGui::PushID(k);
            if (k) ImGui::SameLine(0, 2);
            const bool cur = note == previewNote;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::Orange));
            else if (isBlack(k)) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::rgb(0x202028)));
            else ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(col::rgb(0x3A3A44)));
            if (ImGui::Button(kNoteNames[k], ImVec2(keyW, 0))) {
                previewNote = note;
                app.auditionNote(t.id, note, 1.0f, 1.0);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%d: choose + hear", kNoteNames[k], note / 12 - 1);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::Checkbox("preview on change", &app.previewOnEdit);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the 808 automatically after you change a knob below");
        if (auto proc = app.runtime().processorForSlot(t.instrument->id)) {
            for (int i = 0; i < proc->numParams(); ++i) {
                const auto& pi = proc->paramInfo(i);
                float v = proc->getParam(i);
                ImGui::SetNextItemWidth(220 * dpi);
                if (pi.steps == 2) {
                    bool b = v > 0.5f;
                    if (ImGui::Checkbox(pi.name.c_str(), &b) &&
                        app.run("SetParam", {{"slotId", t.instrument->id}, {"paramId", pi.id}, {"value", b ? 1.0 : 0.0}}))
                        previewTrack = t.id;
                } else if (ImGui::SliderFloat(pi.name.c_str(), &v, pi.minValue, pi.maxValue, "%.2f")) {
                    proc->setParam(i, v); // live while dragging
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && pi.steps != 2 &&
                    app.run("SetParam", {{"slotId", t.instrument->id}, {"paramId", pi.id}, {"value", proc->getParam(i)}}))
                    previewTrack = t.id;
                ImGui::PushID(i);
                if (ImGui::BeginPopupContextItem("p808ctx")) {
                    if (ImGui::MenuItem("Default value"))
                        app.run("SetParam", {{"slotId", t.instrument->id}, {"paramId", pi.id}, {"value", pi.defaultValue}});
                    midiLearnMenuItems(app, t.channelId, t.instrument->id, pi.id, t.name + " · " + pi.name);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                midiMappedTag(app, t.channelId, t.instrument->id, pi.id);
            }
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!previewTrack.empty() && app.previewOnEdit) app.auditionNote(previewTrack, previewNote, 1.0f, 1.0);
    if (!n808) {
        ImGui::TextDisabled("No 808 track yet.");
        if (goldButton("Create 808 track"))
            app.run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"instrument", "roy.808"}, {"role", "808"}});
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("808analyzer", ImVec2(0, 0));
    sectionTitle("KICK <-> 808 ANALYZER");
    static json report;
    if (goldButton("Analyze kick vs 808")) {
        if (app.run("AnalyzeKick808", json::object())) report = app.lastResult();
        else report = json{{"error", app.lastError()}};
    }
    if (report.contains("error")) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "%s", report["error"].get<std::string>().c_str());
    if (report.contains("visual")) {
        const json& v = report["visual"];
        ImGui::Text("kick %.0f Hz | 808 %.0f Hz | 808 starts %.0f ms after the kick", report.value("kickFundamentalHz", 0.0),
                    report.value("bassFundamentalHz", 0.0), report.value("offsetMs", 0.0));
        const double fo = v.value("frequencyOverlap", 0.0), to = v.value("timingOverlapMs", 0.0), pc = v.value("phaseCorrelation", 0.0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(fo > 0.5 ? col::Orange : col::Green), "frequency overlap %.0f %%", fo * 100);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(to > 80 ? col::Orange : col::Green), "| timing overlap %.0f ms", to);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pc < 0 ? col::Red : col::Green), "| phase correlation %+.2f", pc);
        // plot: series of (x, y) with a shared y range; `fill` shades the area both curves share
        auto plot = [&](const char* id, const json& xs, std::initializer_list<std::pair<const json*, ImU32>> series, float yMin, float yMax,
                        bool logX, bool fillShared) {
            const ImVec2 size(ImGui::GetContentRegionAvail().x, 110 * dpi);
            const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, b, col::Panel2, 3);
            ImGui::InvisibleButton(id, size);
            if (!xs.is_array() || xs.size() < 2) return;
            const double x0 = xs.front().get<double>(), x1 = xs.back().get<double>();
            auto X = [&](double x) {
                const double t = logX ? std::log(x / x0) / std::log(x1 / x0) : (x - x0) / (x1 - x0);
                return a.x + static_cast<float>(t) * size.x;
            };
            auto Y = [&](double y) { return b.y - static_cast<float>((std::clamp(y, double(yMin), double(yMax)) - yMin) / (yMax - yMin)) * size.y; };
            if (fillShared && series.size() >= 2) {
                const json& s1 = *series.begin()->first;
                const json& s2 = *(series.begin() + 1)->first;
                for (size_t i = 0; i + 1 < xs.size() && i < s1.size() && i < s2.size(); ++i) {
                    if (s1[i].is_null() || s2[i].is_null()) continue;
                    const double m = std::min(s1[i].get<double>(), s2[i].get<double>());
                    dl->AddRectFilled(ImVec2(X(xs[i].get<double>()), Y(m)), ImVec2(X(xs[i + 1].get<double>()), b.y), col::rgb(0xFF8C00, 70));
                }
            }
            for (auto& [sp, color] : series) {
                const json& sv = *sp;
                for (size_t i = 0; i + 1 < xs.size() && i + 1 < sv.size(); ++i) {
                    if (sv[i].is_null() || sv[i + 1].is_null()) continue;
                    dl->AddLine(ImVec2(X(xs[i].get<double>()), Y(sv[i].get<double>())), ImVec2(X(xs[i + 1].get<double>()), Y(sv[i + 1].get<double>())),
                                color, 1.5f * dpi);
                }
            }
            dl->AddText(ImVec2(a.x + 4, a.y + 2), col::IvoryDim, id);
        };
        const json kE = v["kickEnvDb"], bE = v["bassEnvDb"], corr = v["correlation"];
        plot("low-band envelopes (dB, time)  gold = kick  orange = 808  shaded = both sound", v["timeMs"], {{&kE, col::Gold}, {&bE, col::Orange}}, -48, 0, false, true);
        plot("phase correlation over time (+1 in phase, -1 cancels)", v["timeMs"], {{&corr, col::Green}}, -1, 1, false, false);
        const json kS = v["kickSpecDb"], bS = v["bassSpecDb"];
        plot("spectrum 20-400 Hz  gold = kick  orange = 808  shaded = shared", v["freqHz"], {{&kS, col::Gold}, {&bS, col::Orange}}, -60, 0, true, true);
        for (auto& sug : report.value("suggestions", json::array())) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", sug.value("description", std::string()).c_str());
        }
        if (report.value("suggestions", json::array()).empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "No kick/808 conflict found.");
        ImGui::TextDisabled("Measurement only - nothing in the project is changed.");
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

} // namespace roy::gui
