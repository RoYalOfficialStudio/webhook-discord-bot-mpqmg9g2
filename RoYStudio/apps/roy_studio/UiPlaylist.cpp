#include "Ui.h"

#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <format>

namespace roy::gui {

namespace {
struct Drag {
    std::string clipId;
    std::string trackId;
    double grabOffsetBeats = 0;
    double originalStart = 0;
    bool active = false;
};
Drag g_drag;
std::set<std::string> g_multi; // Ctrl+click multi-selection; dragging one moves all (one undo step)
std::set<std::string> g_expanded; // tracks whose take lanes are shown
struct CompDrag {
    std::string trackId, takeId;
    double startBeat = 0;
    bool active = false;
};
CompDrag g_comp; // swipe comping: drag over a take lane -> that range plays from this take

// Takes in display order (one row each, oldest lane first).
std::vector<const Take*> takeRows(const Track& t) {
    std::vector<const Take*> v;
    for (auto& k : t.takes) v.push_back(&k);
    std::stable_sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->lane < b->lane; });
    return v;
}

// A comp segment / take drawn like an audio clip starting at `start` inside take `k`.
AudioClip takeView(const Project& p, const Take& k, double start, double end) {
    AudioClip c;
    c.assetId = k.assetId;
    c.startBeat = start;
    c.lengthBeats = end - start;
    c.sourceOffsetSec = p.tempo.beatToSeconds(start) - p.tempo.beatToSeconds(k.startBeat);
    return c;
}

const char* typeTag(TrackType t) { return t == TrackType::Audio ? "AUDIO" : t == TrackType::Midi ? "MIDI" : "BEAT"; }

double snap(double beat, double grid) { return grid > 0 ? std::round(beat / grid) * grid : beat; }

void drawWaveform(App& app, ImDrawList* dl, const AudioClip& c, ImVec2 a, ImVec2 b) {
    auto wf = app.waveform(c.assetId);
    if (!wf || wf->numFrames() == 0) return;
    const auto& p = app.project();
    const double sr = app.engine().sampleRate();
    const double srcStart = c.sourceOffsetSec * sr;
    const double secs = (p.tempo.beatToSeconds(c.endBeat()) - p.tempo.beatToSeconds(c.startBeat)) / std::max(1e-6, c.stretch);
    const double srcEnd = srcStart + secs * sr;
    const int px = std::clamp(static_cast<int>(b.x - a.x), 1, 4096);
    static std::vector<float> mins, maxs;
    mins.assign(static_cast<size_t>(px), 0.0f);
    maxs.assign(static_cast<size_t>(px), 0.0f);
    wf->getPeaks(0, srcStart, srcEnd, px, mins.data(), maxs.data());
    const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.45f;
    const float g = std::pow(10.0f, c.gainDb / 20.0f);
    for (int i = 0; i < px; ++i) {
        const float lo = std::clamp(mins[static_cast<size_t>(i)] * g, -1.0f, 1.0f), hi = std::clamp(maxs[static_cast<size_t>(i)] * g, -1.0f, 1.0f);
        dl->AddLine(ImVec2(a.x + i, mid - hi * half), ImVec2(a.x + i, mid - lo * half + 1), col::rgb(0x0B0B0E, 200));
    }
}

void drawMidiPreview(ImDrawList* dl, const MidiClip& c, ImVec2 a, ImVec2 b, double ppb) {
    if (c.notes.empty()) return;
    int lo = 127, hi = 0;
    for (auto& n : c.notes) {
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
    }
    const float h = b.y - a.y - 16;
    const int range = std::max(12, hi - lo + 1);
    for (auto& n : c.notes) {
        if (n.startBeat >= c.lengthBeats) continue;
        const float x0 = a.x + static_cast<float>(n.startBeat * ppb);
        const float x1 = std::min(b.x, a.x + static_cast<float>((n.startBeat + n.lengthBeats) * ppb));
        const float y = a.y + 14 + h * (1.0f - static_cast<float>(n.pitch - lo + 0.5f) / range);
        dl->AddRectFilled(ImVec2(x0, y - 1.5f), ImVec2(std::max(x0 + 2, x1), y + 1.5f), col::rgb(0x0B0B0E, 220));
    }
}

void drawPatternPreview(const Project& p, ImDrawList* dl, const PatternClip& c, ImVec2 a, ImVec2 b, double ppb) {
    const Pattern* pat = p.findPattern(c.patternId);
    if (!pat || pat->rows.empty()) return;
    const float rowH = (b.y - a.y - 16) / static_cast<float>(pat->rows.size());
    const double len = pat->lengthBeats();
    for (double off = 0; off < c.lengthBeats; off += len)
        for (size_t r = 0; r < pat->rows.size(); ++r)
            for (size_t s = 0; s < pat->rows[r].steps.size(); ++s) {
                if (!pat->rows[r].steps[s].on) continue;
                const double beat = off + static_cast<double>(s) * pat->stepLengthBeats;
                if (beat >= c.lengthBeats) break;
                const float x = a.x + static_cast<float>(beat * ppb);
                const float y = a.y + 14 + rowH * static_cast<float>(r);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(2.0f, static_cast<float>(pat->stepLengthBeats * ppb) - 1), y + rowH - 1),
                                  col::rgb(0x0B0B0E, 200));
            }
}
} // namespace

void drawPlaylist(App& app) {
    Project& p = app.project();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    const float headerW = 200 * dpi, rulerH = 22 * dpi, sectionH = 18 * dpi, rowH = 58 * dpi, takeH = 26 * dpi;
    // variable track heights: main row + one row per take when the take lanes are expanded
    std::erase_if(g_expanded, [&](const std::string& id) { return !p.findTrack(id); });
    // a track that gets its second take (or is loaded with several) opens its take lanes once
    static std::map<std::string, size_t> seenTakes;
    for (auto& t : p.tracks) {
        size_t& seen = seenTakes[t.id];
        if (t.takes.size() >= 2 && seen < 2) g_expanded.insert(t.id);
        seen = t.takes.size();
    }
    std::vector<float> rowY(p.tracks.size() + 1, 0.0f);
    auto takeLanes = [&](const Track& t) { return g_expanded.count(t.id) ? t.takes.size() : size_t{0}; };
    for (size_t i = 0; i < p.tracks.size(); ++i) rowY[i + 1] = rowY[i] + rowH + takeH * static_cast<float>(takeLanes(p.tracks[i]));
    const float tracksH = rowY.back();

    // toolbar
    ImGui::TextDisabled("Snap");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * dpi);
    const char* snaps[] = {"off", "1/16", "1/8", "1/4", "1 bar"};
    const double snapVals[] = {0.0, 0.25, 0.5, 1.0, 4.0};
    int si = 1;
    for (int i = 0; i < 5; ++i)
        if (std::fabs(app.snapBeats - snapVals[i]) < 1e-9) si = i;
    if (ImGui::Combo("##snap", &si, snaps, 5)) app.snapBeats = snapVals[si];
    ImGui::SameLine();
    ImGui::TextDisabled("Zoom");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * dpi);
    float ppb = static_cast<float>(app.pixelsPerBeat);
    if (ImGui::SliderFloat("##zoom", &ppb, 4, 160, "%.0f px/beat")) app.pixelsPerBeat = ppb;
    ImGui::SameLine();
    if (ImGui::Button("+ Audio")) app.run("AddTrack", {{"type", "audio"}, {"name", "Audio"}});
    ImGui::SameLine();
    if (ImGui::Button("+ Vocal")) {
        std::string bus;
        for (auto& c : p.channels)
            if (c.name == "VOCALS") bus = c.id;
        app.run("AddTrack", {{"type", "audio"}, {"name", "Vocal"}, {"role", "vocal"}, {"output", bus}});
    }
    ImGui::SameLine();
    if (ImGui::Button("+ MIDI")) app.run("AddTrack", {{"type", "midi"}, {"name", "Synth"}});
    ImGui::SameLine();
    if (ImGui::Button("+ Beat")) app.run("AddTrack", {{"type", "beat"}, {"name", "Drums"}, {"role", "drums"}});
    ImGui::SameLine();
    if (ImGui::Button("+ 808")) app.run("AddTrack", {{"type", "midi"}, {"name", "808"}, {"instrument", "roy.808"}, {"role", "808"}});

    ImGui::BeginChild("timeline", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float scrollY = ImGui::GetScrollY();
    const double endBeat = std::max(64.0, p.endBeat() + 16.0);
    const float contentW = headerW + static_cast<float>(endBeat * app.pixelsPerBeat);
    const float contentH = rulerH + sectionH + tracksH + rowH;
    ImGui::Dummy(ImVec2(contentW, contentH));
    const float scrollX = ImGui::GetScrollX();
    const ImVec2 win0 = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const float laneX0 = origin.x + headerW;
    auto beatToX = [&](double beat) { return laneX0 + static_cast<float>(beat * app.pixelsPerBeat); };
    auto xToBeat = [&](float x) { return std::max(0.0, (x - laneX0) / app.pixelsPerBeat); };

    // grid
    const int num = p.tempo.signatureAtBar(0).numerator;
    const float gridTop = origin.y + rulerH + sectionH;
    const float gridBottom = origin.y + contentH;
    const double firstBeat = std::floor(xToBeat(win0.x + headerW + scrollX - 50));
    const double lastBeat = xToBeat(win0.x + winSize.x + scrollX) + 1;
    for (double b = std::max(0.0, firstBeat); b <= lastBeat; b += 1.0) {
        const bool bar = static_cast<int>(b) % num == 0;
        if (!bar && app.pixelsPerBeat < 10) continue;
        const float x = beatToX(b);
        dl->AddLine(ImVec2(x, gridTop), ImVec2(x, gridBottom), bar ? col::GridBar : col::Grid);
    }
    // track lanes
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        const float y = gridTop + rowY[i], y1 = gridTop + rowY[i + 1];
        if (p.tracks[i].id == app.selTrack) dl->AddRectFilled(ImVec2(laneX0, y), ImVec2(origin.x + contentW, y1), col::rgb(0xD4AF37, 14));
        if (y1 - y > rowH) dl->AddRectFilled(ImVec2(laneX0, y + rowH), ImVec2(origin.x + contentW, y1), col::rgb(0x000000, 60));
        dl->AddLine(ImVec2(laneX0, y1), ImVec2(origin.x + contentW, y1), col::Grid);
    }
    // sections band
    for (auto& s : p.sections) {
        const float x0 = beatToX(s.startBeat), x1 = beatToX(s.endBeat);
        const ImVec2 a(x0, origin.y + scrollY + rulerH), b(x1 - 1, origin.y + scrollY + rulerH + sectionH - 2);
        dl->AddRectFilled(a, b, clipColor(s.type == "hook" ? 0xFF8C00 : s.type == "verse" ? 0x3CB371 : 0xD4AF37, 150), 3);
        dl->AddText(ImVec2(x0 + 4, a.y + 1), col::Obsidian, s.name.c_str());
    }

    // clips
    std::string hoverClip;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        const Track& t = p.tracks[i];
        const float y0 = gridTop + rowY[i] + 2, y1 = y0 + rowH - 4;
        auto clipRect = [&](const std::string& id, double start, double len, uint32_t color, bool muted, const std::string& name, auto&& body) {
            double s = start;
            if (g_drag.active && (g_drag.clipId == id || (g_multi.count(g_drag.clipId) && g_multi.count(id)))) {
                const double delta = snap(xToBeat(mouse.x) - g_drag.grabOffsetBeats, app.snapBeats) - g_drag.originalStart;
                s = std::max(0.0, start + delta);
            }
            const ImVec2 a(beatToX(s), y0), b(beatToX(s + len), y1);
            if (b.x < win0.x || a.x > win0.x + winSize.x + scrollX + 200) return;
            const bool sel = id == app.selClip || id == app.selMidiClip || g_multi.count(id);
            dl->AddRectFilled(a, b, clipColor(color, muted ? 70 : 215), 4);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 14), clipColor(color, 255), 4, ImDrawFlags_RoundCornersTop);
            body(a, b);
            dl->AddRect(a, b, sel ? col::Ivory : col::rgb(0x000000, 120), 4, 0, sel ? 2.0f : 1.0f);
            dl->PushClipRect(a, b, true);
            dl->AddText(ImVec2(a.x + 4, a.y), col::Obsidian, name.c_str());
            dl->PopClipRect();
            if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) hoverClip = id;
        };
        for (auto& c : t.audioClips)
            clipRect(c.id, c.startBeat, c.lengthBeats, c.color, c.muted, c.name, [&](ImVec2 a, ImVec2 b) { drawWaveform(app, dl, c, ImVec2(a.x, a.y + 14), b); });
        for (auto& c : t.midiClips)
            clipRect(c.id, c.startBeat, c.lengthBeats, c.color, c.muted, c.name.empty() ? "MIDI" : c.name,
                     [&](ImVec2 a, ImVec2 b) { drawMidiPreview(dl, c, a, b, app.pixelsPerBeat); });
        for (auto& c : t.patternClips) {
            const Pattern* pat = p.findPattern(c.patternId);
            clipRect(c.id, c.startBeat, c.lengthBeats, c.color, c.muted, pat ? pat->name : "Pattern",
                     [&](ImVec2 a, ImVec2 b) { drawPatternPreview(p, dl, c, a, b, app.pixelsPerBeat); });
        }
        // the comp plays together with the regular clips, so it is always drawn
        for (auto& seg : t.comp) {
            const Take* k = nullptr;
            for (auto& x : t.takes)
                if (x.id == seg.takeId) k = &x;
            const ImVec2 a(beatToX(seg.startBeat), y0), b(beatToX(seg.endBeat), y1);
            if (b.x < win0.x || a.x > win0.x + winSize.x + scrollX + 200) continue;
            dl->AddRectFilled(a, b, col::rgb(0xFF8C00, 170), 4);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 14), col::rgb(0xFF8C00, 255), 4, ImDrawFlags_RoundCornersTop);
            if (k) drawWaveform(app, dl, takeView(p, *k, seg.startBeat, seg.endBeat), ImVec2(a.x, a.y + 14), b);
            dl->AddRect(a, b, col::rgb(0x000000, 120), 4);
            dl->PushClipRect(a, b, true);
            dl->AddText(ImVec2(a.x + 4, a.y), col::Obsidian, k ? k->name.c_str() : "comp");
            dl->PopClipRect();
        }
        // take lanes: every take in full, the parts that are in the comp highlighted
        if (takeLanes(t)) {
            const auto rows = takeRows(t);
            for (size_t r = 0; r < rows.size(); ++r) {
                const Take& k = *rows[r];
                const float ty0 = gridTop + rowY[i] + rowH + takeH * static_cast<float>(r) + 1, ty1 = ty0 + takeH - 2;
                const ImVec2 a(beatToX(k.startBeat), ty0), b(beatToX(k.startBeat + k.lengthBeats), ty1);
                if (b.x < win0.x || a.x > win0.x + winSize.x + scrollX + 200) continue;
                dl->AddRectFilled(a, b, col::rgb(0x6A6A78, 110), 3);
                for (auto& seg : t.comp)
                    if (seg.takeId == k.id) dl->AddRectFilled(ImVec2(beatToX(seg.startBeat), ty0), ImVec2(beatToX(seg.endBeat), ty1), col::rgb(0xFF8C00, 190), 3);
                drawWaveform(app, dl, takeView(p, k, k.startBeat, k.startBeat + k.lengthBeats), a, b);
                if (g_comp.active && g_comp.takeId == k.id) {
                    const double m = std::clamp(snap(xToBeat(mouse.x), app.snapBeats), k.startBeat, k.startBeat + k.lengthBeats);
                    const double c0 = std::clamp(g_comp.startBeat, k.startBeat, k.startBeat + k.lengthBeats);
                    dl->AddRectFilled(ImVec2(beatToX(std::min(m, c0)), ty0), ImVec2(beatToX(std::max(m, c0)), ty1), col::rgb(0xFFF0C8, 90), 3);
                }
                dl->AddRect(a, b, col::rgb(0x000000, 140), 3);
            }
        }
    }

    // interactions on the lane area
    ImGui::SetCursorScreenPos(ImVec2(laneX0, gridTop));
    ImGui::InvisibleButton("lanes", ImVec2(std::max(1.0f, contentW - headerW), std::max(1.0f, tracksH + rowH)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    // hit test: which track, and which take row (-1 = the track's main row)
    const Track* laneTrack = nullptr;
    const Take* laneTake = nullptr;
    {
        const float my = mouse.y - gridTop;
        for (size_t i = 0; i < p.tracks.size(); ++i)
            if (my >= rowY[i] && my < rowY[i + 1]) {
                laneTrack = &p.tracks[i];
                if (my - rowY[i] >= rowH) {
                    const auto rows = takeRows(*laneTrack);
                    const size_t r = static_cast<size_t>((my - rowY[i] - rowH) / takeH);
                    if (r < rows.size()) laneTake = rows[r];
                }
            }
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && laneTake) {
        g_comp = {laneTrack->id, laneTake->id, snap(xToBeat(mouse.x), app.snapBeats), true};
        app.selTrack = laneTrack->id;
        app.selChannel = laneTrack->channelId;
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (ImGui::GetIO().KeyCtrl && !hoverClip.empty()) {
            if (!g_multi.erase(hoverClip)) g_multi.insert(hoverClip);
        } else if (!g_multi.count(hoverClip)) {
            g_multi.clear(); // plain click outside the selection starts a new one
        }
        if (!hoverClip.empty()) {
            Track* owner = nullptr;
            double start = 0;
            if (auto* c = p.findAudioClip(hoverClip, &owner)) {
                start = c->startBeat;
                app.selClip = c->id;
            } else if (auto* m = p.findMidiClip(hoverClip, &owner)) {
                start = m->startBeat;
                app.selMidiClip = m->id;
            } else {
                for (auto& t : p.tracks)
                    for (auto& pc : t.patternClips)
                        if (pc.id == hoverClip) {
                            start = pc.startBeat;
                            app.selPattern = pc.patternId;
                        }
            }
            g_drag = {hoverClip, laneTrack ? laneTrack->id : "", xToBeat(mouse.x) - start, start, true};
        }
        if (laneTrack) {
            app.selTrack = laneTrack->id;
            app.selChannel = laneTrack->channelId;
        }
    }
    if (g_comp.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const double e = snap(xToBeat(mouse.x), app.snapBeats);
        const double a = std::min(g_comp.startBeat, e), b = std::max(g_comp.startBeat, e);
        if (b - a > 1e-6) app.run("CompSelect", {{"trackId", g_comp.trackId}, {"takeId", g_comp.takeId}, {"startBeat", a}, {"endBeat", b}});
        g_comp.active = false;
    }
    if (g_drag.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const double ns = snap(xToBeat(mouse.x) - g_drag.grabOffsetBeats, app.snapBeats);
        const std::string target = laneTrack ? laneTrack->id : "";
        if (g_multi.size() > 1 && g_multi.count(g_drag.clipId)) {
            if (std::fabs(ns - g_drag.originalStart) > 1e-9)
                app.run("MoveClips", {{"clipIds", std::vector<std::string>(g_multi.begin(), g_multi.end())}, {"deltaBeats", ns - g_drag.originalStart}});
        } else if (std::fabs(ns - g_drag.originalStart) > 1e-9 || (!target.empty() && target != g_drag.trackId)) {
            app.run("MoveClip", {{"clipId", g_drag.clipId}, {"startBeat", ns}, {"trackId", target == g_drag.trackId ? "" : target}});
        }
        g_drag.active = false;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (laneTake) {
            app.run("CompWholeTake", {{"trackId", laneTrack->id}, {"takeId", laneTake->id}});
        } else if (!hoverClip.empty() && p.findMidiClip(hoverClip)) {
            app.selMidiClip = hoverClip;
            app.area = Area::PianoRoll;
        } else if (hoverClip.empty() && laneTrack && laneTrack->type == TrackType::Midi) {
            app.run("AddMidiClip", {{"trackId", laneTrack->id}, {"startBeat", snap(xToBeat(mouse.x), 4.0)}, {"lengthBeats", 4.0}});
            app.selMidiClip = app.lastResult().value("id", "");
        } else if (hoverClip.empty() && laneTrack && laneTrack->type == TrackType::Beat && !app.selPattern.empty()) {
            app.run("AddPatternClip", {{"trackId", laneTrack->id}, {"patternId", app.selPattern}, {"startBeat", snap(xToBeat(mouse.x), 4.0)}});
        }
    }
    static std::string ctxClip;
    static double ctxBeat = 0;
    static std::string ctxTrack, ctxTake;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && laneTake) {
        ctxTrack = laneTrack->id;
        ctxTake = laneTake->id;
        ImGui::OpenPopup("takeMenu");
    }
    if (ImGui::BeginPopup("takeMenu")) {
        Track* tt = p.findTrack(ctxTrack);
        const Take* tk = nullptr;
        if (tt)
            for (auto& k : tt->takes)
                if (k.id == ctxTake) tk = &k;
        if (!tk) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("%s", tk->name.c_str());
            if (ImGui::MenuItem("Use whole take")) app.run("CompWholeTake", {{"trackId", ctxTrack}, {"takeId", ctxTake}});
            static char tname[96];
            if (ImGui::IsWindowAppearing()) std::snprintf(tname, sizeof(tname), "%s", tk->name.c_str());
            ImGui::SetNextItemWidth(160 * dpi);
            ImGui::InputText("##takename", tname, sizeof(tname));
            ImGui::SameLine();
            if (ImGui::Button("Rename")) {
                app.run("RenameTake", {{"trackId", ctxTrack}, {"takeId", ctxTake}, {"name", std::string(tname)}});
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Flatten comp to clips", nullptr, false, !tt->comp.empty())) app.run("FlattenComp", {{"trackId", ctxTrack}});
            if (ImGui::MenuItem("Clear comp", nullptr, false, !tt->comp.empty())) app.run("ClearComp", {{"trackId", ctxTrack}});
            if (ImGui::MenuItem("Delete take (file stays in the project folder)")) app.run("DeleteTake", {{"trackId", ctxTrack}, {"takeId", ctxTake}});
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !hoverClip.empty() && !laneTake) {
        ctxClip = hoverClip;
        ctxBeat = snap(xToBeat(mouse.x), app.snapBeats);
        ImGui::OpenPopup("clipMenu");
    }
    if (ImGui::BeginPopup("clipMenu")) {
        if (ImGui::MenuItem("Split here")) app.run("SplitClip", {{"clipId", ctxClip}, {"atBeat", ctxBeat}});
        if (ImGui::MenuItem("Duplicate")) app.run("DuplicateClip", {{"clipId", ctxClip}});
        if (auto* c = p.findAudioClip(ctxClip)) {
            if (ImGui::MenuItem(c->muted ? "Unmute" : "Mute")) app.run("MuteClip", {{"clipId", ctxClip}, {"muted", !c->muted}});
            if (ImGui::MenuItem("Normalize")) app.run("NormalizeClip", {{"clipId", ctxClip}});
            if (ImGui::MenuItem("Open in VOCALS")) {
                app.selClip = ctxClip;
                app.area = Area::Vocals;
            }
        }
        if (p.findMidiClip(ctxClip) && ImGui::MenuItem("Open in PIANO ROLL")) {
            app.selMidiClip = ctxClip;
            app.area = Area::PianoRoll;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) app.run("DeleteClip", {{"clipId", ctxClip}});
        ImGui::EndPopup();
    }
    // drag & drop from the browser: audio files onto audio tracks
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ROY_FILE")) {
            const std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
            if (laneTrack && laneTrack->type == TrackType::Midi) { // Browser -> Sampler (MIDI track)
                const std::string midiTrack = laneTrack->id;
                const std::string asset = app.importAsset(path);
                if (!asset.empty()) app.run("LoadSampleIntoSampler", {{"trackId", midiTrack}, {"assetId", asset}, {"rootNote", 60}, {"autoRoot", true}});
                ImGui::EndDragDropTarget();
                ImGui::EndChild();
                return;
            }
            std::string trackId = laneTrack && laneTrack->type == TrackType::Audio ? laneTrack->id : "";
            if (trackId.empty()) {
                app.run("AddTrack", {{"type", "audio"}, {"name", "Audio"}});
                trackId = app.lastResult().value("id", "");
            }
            app.run("ImportAudio", {{"path", path}, {"trackId", trackId}, {"startBeat", snap(xToBeat(mouse.x), app.snapBeats)}});
        }
        ImGui::EndDragDropTarget();
    }

    // ruler (sticky at top) + playhead
    {
        const float ry = origin.y + scrollY;
        dl->AddRectFilled(ImVec2(win0.x, ry), ImVec2(win0.x + winSize.x + scrollX + contentW, ry + rulerH), col::Panel2);
        for (double b = std::max(0.0, firstBeat); b <= lastBeat; b += 1.0) {
            const int ib = static_cast<int>(b);
            if (ib % num) continue;
            const float x = beatToX(b);
            dl->AddLine(ImVec2(x, ry + rulerH * 0.5f), ImVec2(x, ry + rulerH), col::GoldDim);
            if (app.pixelsPerBeat * num > 28 || (ib / num) % 4 == 0) dl->AddText(ImVec2(x + 3, ry + 3), col::IvoryDim, std::to_string(ib / num + 1).c_str());
        }
        for (auto& m : p.markers) {
            const float x = beatToX(m.beat);
            dl->AddTriangleFilled(ImVec2(x - 5, ry), ImVec2(x + 5, ry), ImVec2(x, ry + 8), col::Orange);
            dl->AddText(ImVec2(x + 6, ry + 4), col::Orange, m.name.c_str());
        }
        if (p.loop.enabled)
            dl->AddRectFilled(ImVec2(beatToX(p.loop.startBeat), ry + rulerH - 5), ImVec2(beatToX(p.loop.endBeat), ry + rulerH), col::rgb(0xFF8C00, 180));
        ImGui::SetCursorScreenPos(ImVec2(laneX0, ry));
        ImGui::InvisibleButton("ruler", ImVec2(std::max(1.0f, contentW - headerW), rulerH));
        if (ImGui::IsItemActive()) app.seekBeat(snap(xToBeat(ImGui::GetIO().MousePos.x), app.snapBeats));
        const float px = beatToX(app.positionBeats());
        dl->AddLine(ImVec2(px, ry), ImVec2(px, gridBottom), col::Gold, 2.0f);
        dl->AddTriangleFilled(ImVec2(px - 6, ry), ImVec2(px + 6, ry), ImVec2(px, ry + 9), col::Gold);
        // follow playhead while playing
        if (app.engine().transport().isPlaying() && (px > win0.x + winSize.x - 40 || px < win0.x + headerW))
            ImGui::SetScrollX(std::max(0.0f, px - origin.x - headerW - 40));
    }

    // track headers (sticky at left)
    {
        const float hx = origin.x + scrollX;
        dl->AddRectFilled(ImVec2(hx, origin.y + scrollY), ImVec2(hx + headerW, gridBottom + rowH), col::Panel);
        for (size_t i = 0; i < p.tracks.size(); ++i) {
            Track& t = p.tracks[i];
            const float y = gridTop + rowY[i];
            const MixerChannel* ch = p.findChannel(t.channelId);
            dl->AddRectFilled(ImVec2(hx, y + 1), ImVec2(hx + 5, y + rowH - 1), clipColor(t.color));
            ImGui::PushID(t.id.c_str());
            ImGui::SetCursorScreenPos(ImVec2(hx + 10, y + 5));
            if (ImGui::Selectable(t.name.c_str(), app.selTrack == t.id, 0, ImVec2(headerW - 70, 0))) {
                app.selTrack = t.id;
                app.selChannel = t.channelId;
            }
            ImGui::SetCursorScreenPos(ImVec2(hx + headerW - 56, y + 5));
            ImGui::TextDisabled("%s", typeTag(t.type));
            ImGui::SetCursorScreenPos(ImVec2(hx + 10, y + rowH - ImGui::GetFrameHeight() - 5));
            const ImVec2 bs(24 * dpi, 0);
            if (ch && toggleButton("M", ch->mute, col::Orange, bs)) app.run("MuteChannel", {{"channelId", ch->id}, {"mute", !ch->mute}});
            ImGui::SameLine();
            if (ch && toggleButton("S", ch->solo, col::Gold, bs)) app.run("SoloChannel", {{"channelId", ch->id}, {"solo", !ch->solo}});
            if (t.type == TrackType::Audio) {
                ImGui::SameLine();
                if (toggleButton("R", t.armed, col::Red, bs)) app.run("ArmTrack", {{"trackId", t.id}, {"armed", !t.armed}});
                ImGui::SameLine();
                if (toggleButton("IN", t.monitor, col::Green, ImVec2(30 * dpi, 0))) app.run("MonitorTrack", {{"trackId", t.id}, {"monitor", !t.monitor}});
                if (!t.takes.empty()) {
                    ImGui::SameLine();
                    const bool open = g_expanded.count(t.id) > 0;
                    if (toggleButton(std::format("T{}", t.takes.size()).c_str(), open, col::Orange, ImVec2(34 * dpi, 0))) {
                        if (open) g_expanded.erase(t.id);
                        else g_expanded.insert(t.id);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show take lanes: drag over a take to comp that range, double-click = whole take");
                }
            } else if (t.instrument) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", t.instrument->name.c_str());
            }
            if (ImGui::BeginPopupContextItem("trackMenu")) {
                static char name[128];
                if (ImGui::IsWindowAppearing()) std::snprintf(name, sizeof(name), "%s", t.name.c_str());
                ImGui::InputText("Name", name, sizeof(name));
                if (ImGui::Button("Rename")) {
                    app.run("RenameTrack", {{"trackId", t.id}, {"name", std::string(name)}});
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Delete track")) app.run("DeleteTrack", {{"trackId", t.id}});
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (takeLanes(t)) {
                const auto rows = takeRows(t);
                for (size_t r = 0; r < rows.size(); ++r) {
                    const bool used = std::any_of(t.comp.begin(), t.comp.end(), [&](auto& s) { return s.takeId == rows[r]->id; });
                    const float ty = y + rowH + takeH * static_cast<float>(r);
                    dl->AddText(ImVec2(hx + 18, ty + (takeH - ImGui::GetFontSize()) * 0.5f), used ? col::Orange : col::IvoryDim,
                                std::format("{} {}", used ? ">" : " ", rows[r]->name).c_str());
                }
            }
            dl->AddLine(ImVec2(hx, gridTop + rowY[i + 1]), ImVec2(hx + headerW, gridTop + rowY[i + 1]), col::Grid);
        }
        dl->AddLine(ImVec2(hx + headerW, origin.y + scrollY), ImVec2(hx + headerW, gridBottom + rowH), col::GoldDim);
        dl->AddRectFilled(ImVec2(hx, origin.y + scrollY), ImVec2(hx + headerW, origin.y + scrollY + rulerH + sectionH), col::Panel2);
        dl->AddText(ImVec2(hx + 10, origin.y + scrollY + 4), col::Gold, std::format("{} tracks", p.tracks.size()).c_str());
    }
    // ctrl + wheel zoom
    if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0)
        app.pixelsPerBeat = std::clamp(app.pixelsPerBeat * (ImGui::GetIO().MouseWheel > 0 ? 1.15 : 1 / 1.15), 4.0, 160.0);
    ImGui::EndChild();
}

} // namespace roy::gui
