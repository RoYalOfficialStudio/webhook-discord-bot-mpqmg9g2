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

// Shared widgets
bool goldButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool toggleButton(const char* label, bool on, ImU32 onColor, const ImVec2& size = ImVec2(0, 0));
void sectionTitle(const char* text);
void levelMeter(float peakL, float peakR, const ImVec2& size);
ImU32 clipColor(uint32_t rgb, int alpha = 255);
// Context-menu entries "MIDI Learn" / "Remove MIDI mapping (CC n)" for a parameter.
void midiLearnMenuItems(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId, const std::string& label);
// Small "CC n" tag after a mapped control (nothing if unmapped). Returns true if mapped.
bool midiMappedTag(App& app, const std::string& channelId, const std::string& slotId, const std::string& paramId);

} // namespace roy::gui
