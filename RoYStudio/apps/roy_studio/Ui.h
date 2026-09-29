#pragma once
// RoY Studio user interface (Dear ImGui). One draw function per area.
#include "App.h"

#include <imgui.h>

namespace roy::gui {

// ---- theme: obsidian / royal gold / orange / ivory / green -----------------------
namespace col {
constexpr ImU32 rgb(uint32_t c, int a = 255) {
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, a);
}
inline const ImU32 Obsidian = rgb(0x0B0B0E);
inline const ImU32 Panel = rgb(0x15151A);
inline const ImU32 Panel2 = rgb(0x1E1E25);
inline const ImU32 Grid = rgb(0x2A2A33);
inline const ImU32 GridBar = rgb(0x3C3C48);
inline const ImU32 Gold = rgb(0xD4AF37);
inline const ImU32 GoldDim = rgb(0x8A7224);
inline const ImU32 Orange = rgb(0xFF8C00);
inline const ImU32 Ivory = rgb(0xFFFFF0);
inline const ImU32 IvoryDim = rgb(0xB8B8AC);
inline const ImU32 Green = rgb(0x3CB371);
inline const ImU32 Red = rgb(0xE0453A);
} // namespace col

void applyTheme(float dpiScale);
void loadFonts(float dpiScale);

// Main frame: menu, transport, browser sidebar, area tabs, status bar.
void drawStudio(App& app);

void drawTransport(App& app);
void drawBrowser(App& app);
void drawPlaylist(App& app);
void drawChannels(App& app);
void drawPianoRoll(App& app);
void drawMixer(App& app);
void drawVocals(App& app);
void drawBeats(App& app);
void drawPlugins(App& app);
void drawMaster(App& app);
void drawProject(App& app);
void drawPalette(App& app);
void drawSetupWizard(App& app);   // first start / Audio > Setup check
void drawSystemCheck(App& app);   // Help > System check
void drawMusicSession(App& app);  // Help > FIRST REAL MUSIC SESSION
void drawImportBeat(App& app);    // IMPORT BEAT window (MP3 / WAV beat -> own track, tempo, key)

// Shared widgets
bool goldButton(const char* label, const ImVec2& size = ImVec2(0, 0));
// For a slider that shows a project value re-read every frame: on the frame the mouse is released
// the widget no longer writes its value, so the local variable holds the OLD value again. Call
// right after the widget: returns true once when the edit is finished, `v` = the final value.
bool editFinished(float& v);
bool editFinished(int& v);
bool toggleButton(const char* label, bool on, ImU32 onColor, const ImVec2& size = ImVec2(0, 0));
void sectionTitle(const char* text);
void levelMeter(float peakL, float peakR, const ImVec2& size);
ImU32 clipColor(uint32_t rgb, int alpha = 255);
// Context-menu entries "MIDI Learn" / "Remove MIDI mapping (CC n)" for a parameter.
void midiLearnMenuItems(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId, const std::string& label);
// Small "CC n" tag after a mapped control (nothing if unmapped). Returns true if mapped.
bool midiMappedTag(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId);
// Project tempo: drag left/right, double-click to type, mouse wheel +-1 (Shift +-0.1), optional -/+ buttons.
// Every change is one undoable "Set Tempo". Returns true when the tempo was changed.
bool tempoField(App& app, const char* id, float width, bool stepButtons);
// Makes the last item a drag source for a pattern (payload "ROY_PATTERN" = pattern id);
// dropped on a PLAYLIST lane it becomes a pattern clip.
void patternDragSource(const std::string& patternId, const std::string& name);
// Menu entries (inside an open popup): load a RoY / your channel preset, save this channel as preset.
void channelPresetMenu(App& app, const std::string& channelId, const std::string& suggestedName);
// LIVE VOCAL button of an audio track (monitor + autotune); right-click: speed, strength, presets.
void liveVocalButton(App& app, const std::string& trackId, const ImVec2& size);

} // namespace roy::gui
