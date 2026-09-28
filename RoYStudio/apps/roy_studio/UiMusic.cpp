// PIANO ROLL, CHANNELS (step sequencer), BEATS (patterns, 808).
#include "Ui.h"

#include "midi/Scale.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::gui {

namespace {
const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
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
    // canvas interaction
    ImGui::SetCursorScreenPos(ImVec2(x0, o.y));
    ImGui::InvisibleButton("canvas", ImVec2(std::max(1.0f, W - keysW), H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const int mp = 127 - static_cast<int>((mouse.y - o.y) / h);
    const double mb = std::floor(((mouse.x - x0) / ppb) / gridBeats) * gridBeats;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hover >= 0) {
            if (!ImGui::GetIO().KeyShift) selection.clear();
            selection.push_back(static_cast<size_t>(hover));
        } else if (mp >= 0 && mp < 128 && mb >= 0 && mb < clip->lengthBeats) {
            selection.clear();
            app.run("AddNote", {{"clipId", clip->id}, {"pitch", mp}, {"startBeat", mb}, {"lengthBeats", gridBeats * (gridBeats < 0.5 ? 2 : 1)}, {"velocity", 100}});
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
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        app.run("AddPattern", {{"name", std::format("Pattern {}", p.patterns.size() + 1)}, {"steps", pat->numSteps}});
        app.selPattern = app.lastResult().value("id", app.selPattern);
        return;
    }
    ImGui::SameLine();
    float swing = pat->swing;
    ImGui::SetNextItemWidth(140 * dpi);
    ImGui::SliderFloat("Swing", &swing, 0, 1, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) app.run("SetPatternSwing", {{"patternId", pat->id}, {"swing", swing}});
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
        if (!row.sampleAssetId.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            app.run("SetRowSample", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"assetId", ""}});
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
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && st.on)
                app.run("SetStep", {{"patternId", pid}, {"row", static_cast<int>(r)}, {"step", static_cast<int>(s)}, {"velocity", st.velocity < 0.6f ? 1.0 : 0.45}});
            ImGui::PopID();
            if (static_cast<int>(r) < 0) break;
        }
        ImGui::PopID();
        if (app.project().findPattern(pid) != pat) break; // pattern vector changed (undo etc.)
    }
    ImGui::TextDisabled("Left-click: step on/off  |  right-click: accent/ghost  |  steps honour swing, probability, rolls (BEATS)");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- BEATS (patterns, 808 lab)
void drawBeats(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginChild("patterns", ImVec2(320 * dpi, 0), ImGuiChildFlags_Borders);
    sectionTitle("PATTERNS");
    for (auto& pat : p.patterns) {
        int steps = 0;
        for (auto& r : pat.rows)
            for (auto& s : r.steps) steps += s.on;
        if (ImGui::Selectable(std::format("{}   ({} steps, {} hits)", pat.name, pat.numSteps, steps).c_str(), pat.id == app.selPattern))
            app.selPattern = pat.id;
    }
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
    for (auto& g : grooves) {
        if (ImGui::Button(g.name) && sel) {
            app.run("SetRowPattern", {{"patternId", sel->id}, {"voice", "kick"}, {"text", g.kick}});
            app.run("SetRowPattern", {{"patternId", sel->id}, {"voice", "snare"}, {"text", g.snare}});
            app.run("SetRowPattern", {{"patternId", sel->id}, {"voice", "closed_hat"}, {"text", g.hat}});
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("808lab", ImVec2(0, 0), ImGuiChildFlags_Borders);
    sectionTitle("808 LAB");
    int n808 = 0;
    for (auto& t : p.tracks) {
        if (!t.instrument || t.instrument->typeId != "roy.808") continue;
        ++n808;
        ImGui::PushID(t.id.c_str());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "%s", t.name.c_str());
        if (auto proc = app.runtime().processorForSlot(t.instrument->id)) {
            for (int i = 0; i < proc->numParams(); ++i) {
                const auto& pi = proc->paramInfo(i);
                float v = proc->getParam(i);
                ImGui::SetNextItemWidth(220 * dpi);
                if (pi.steps == 2) {
                    bool b = v > 0.5f;
                    if (ImGui::Checkbox(pi.name.c_str(), &b))
                        app.run("SetParam", {{"slotId", t.instrument->id}, {"paramId", pi.id}, {"value", b ? 1.0 : 0.0}});
                } else if (ImGui::SliderFloat(pi.name.c_str(), &v, pi.minValue, pi.maxValue, "%.2f")) {
                    proc->setParam(i, v); // live while dragging
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && pi.steps != 2)
                    app.run("SetParam", {{"slotId", t.instrument->id}, {"paramId", pi.id}, {"value", proc->getParam(i)}});
            }
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!n808) {
        ImGui::TextDisabled("No 808 track yet.");
        if (goldButton("Create 808 track"))
            app.run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"instrument", "roy.808"}, {"role", "808"}});
    }
    ImGui::EndChild();
}

} // namespace roy::gui
