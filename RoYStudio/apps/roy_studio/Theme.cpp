#include "Ui.h"

#include <filesystem>

namespace roy::gui {

void applyTheme(float dpi) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 4;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.ScrollbarRounding = 3;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(6, 5);
    s.ScrollbarSize = 12;
    ImVec4* c = s.Colors;
    auto v = [](uint32_t rgb, float a = 1.0f) {
        return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
    };
    c[ImGuiCol_Text] = v(0xF4F1E6);
    c[ImGuiCol_TextDisabled] = v(0x807C70);
    c[ImGuiCol_WindowBg] = v(0x0B0B0E);
    c[ImGuiCol_ChildBg] = v(0x121216);
    c[ImGuiCol_PopupBg] = v(0x16161B, 0.98f);
    c[ImGuiCol_Border] = v(0x2C2A24);
    c[ImGuiCol_FrameBg] = v(0x1E1E25);
    c[ImGuiCol_FrameBgHovered] = v(0x2A2A33);
    c[ImGuiCol_FrameBgActive] = v(0x33302A);
    c[ImGuiCol_TitleBg] = v(0x0B0B0E);
    c[ImGuiCol_TitleBgActive] = v(0x15151A);
    c[ImGuiCol_MenuBarBg] = v(0x0E0E11);
    c[ImGuiCol_ScrollbarBg] = v(0x0B0B0E);
    c[ImGuiCol_ScrollbarGrab] = v(0x3A382F);
    c[ImGuiCol_ScrollbarGrabHovered] = v(0x8A7224);
    c[ImGuiCol_ScrollbarGrabActive] = v(0xD4AF37);
    c[ImGuiCol_CheckMark] = v(0xD4AF37);
    c[ImGuiCol_SliderGrab] = v(0xD4AF37);
    c[ImGuiCol_SliderGrabActive] = v(0xFFD35A);
    c[ImGuiCol_Button] = v(0x22222A);
    c[ImGuiCol_ButtonHovered] = v(0x3A3524);
    c[ImGuiCol_ButtonActive] = v(0x8A7224);
    c[ImGuiCol_Header] = v(0x2A271E);
    c[ImGuiCol_HeaderHovered] = v(0x3A3524);
    c[ImGuiCol_HeaderActive] = v(0x8A7224);
    c[ImGuiCol_Separator] = v(0x2C2A24);
    c[ImGuiCol_SeparatorHovered] = v(0x8A7224);
    c[ImGuiCol_SeparatorActive] = v(0xD4AF37);
    c[ImGuiCol_ResizeGrip] = v(0x2C2A24);
    c[ImGuiCol_ResizeGripHovered] = v(0x8A7224);
    c[ImGuiCol_ResizeGripActive] = v(0xD4AF37);
    c[ImGuiCol_Tab] = v(0x15151A);
    c[ImGuiCol_TabHovered] = v(0x3A3524);
    c[ImGuiCol_TabSelected] = v(0x2A271E);
    c[ImGuiCol_TabSelectedOverline] = v(0xD4AF37);
    c[ImGuiCol_TableHeaderBg] = v(0x1A1A20);
    c[ImGuiCol_TableBorderStrong] = v(0x2C2A24);
    c[ImGuiCol_TableBorderLight] = v(0x22221C);
    c[ImGuiCol_TableRowBg] = v(0x121216);
    c[ImGuiCol_TableRowBgAlt] = v(0x16161B);
    c[ImGuiCol_TextSelectedBg] = v(0xD4AF37, 0.35f);
    c[ImGuiCol_DragDropTarget] = v(0xFF8C00);
    c[ImGuiCol_NavCursor] = v(0xD4AF37);
    c[ImGuiCol_PlotHistogram] = v(0xD4AF37);
    c[ImGuiCol_PlotLines] = v(0xFF8C00);
    s.ScaleAllSizes(dpi);
}

void loadFonts(float dpi) {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    for (const char* f : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(f, ec)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            if (io.Fonts->AddFontFromFileTTF(f, 15.0f * dpi, &cfg)) return;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * dpi;
    io.Fonts->AddFontDefault(&cfg);
}

ImU32 clipColor(uint32_t rgb, int alpha) { return col::rgb(rgb, alpha); }

namespace {
template <typename T> bool editFinishedImpl(T& v) {
    static ImGuiID activeId = 0; // only one widget can be active at a time
    static T last{};
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemActive()) {
        activeId = id;
        last = v;
    }
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    if (activeId == id) v = last;
    activeId = 0;
    return true;
}
} // namespace
bool editFinished(float& v) { return editFinishedImpl(v); }
bool editFinished(int& v) { return editFinishedImpl(v); }

bool goldButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.83f, 0.69f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.83f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.55f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool toggleButton(const char* label, bool on, ImU32 onColor, const ImVec2& size) {
    if (on) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(onColor);
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 1.1f, c.y * 1.1f, c.z * 1.1f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
    }
    const bool r = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor(3);
    return r;
}

void sectionTitle(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Gold));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void levelMeter(float peakL, float peakR, const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::Obsidian, 2);
    auto bar = [&](float peak, float x0, float x1) {
        const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -90.0f;
        const float norm = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f); // -60 .. +6 dB
        const float top = p.y + size.y * (1.0f - norm);
        const ImU32 c = db > -0.1f ? col::Red : db > -6.0f ? col::Orange : col::Green;
        dl->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(x1, p.y + size.y), c, c, col::GoldDim, col::GoldDim);
    };
    const float w = (size.x - 3) / 2;
    bar(peakL, p.x + 1, p.x + 1 + w);
    bar(peakR, p.x + 2 + w, p.x + 2 + 2 * w);
    const float zero = p.y + size.y * (6.0f / 66.0f);
    dl->AddLine(ImVec2(p.x, zero), ImVec2(p.x + size.x, zero), col::IvoryDim);
    ImGui::Dummy(size);
}

} // namespace roy::gui
