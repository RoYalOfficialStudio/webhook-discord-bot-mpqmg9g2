// RoY Studio - GLFW + OpenGL 3 entry point (Linux/macOS development and CI screenshots).
#include "Options.h"
#include "PngWriter.h"
#include "Ui.h"

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

#include "core/Files.h"

using namespace roy::gui;
using roy::json;
using roy::TrackType;

int main(int argc, char** argv) {
    LaunchOptions lo = parseLaunch(argc, argv);
    if ((!lo.screenshotDir.empty() || !lo.selfTestDir.empty() || lo.benchUiTracks > 0) && lo.app.audioBackend == "auto") lo.app.audioBackend = "null";
    if (!lo.selfTestDir.empty()) { // headless: no window needed
        App app;
        app.init(lo.app);
        const bool ok = app.selfTest(lo.selfTestDir);
        app.shutdown();
        return ok ? 0 : 1;
    }
    glfwSetErrorCallback([](int e, const char* d) { std::fprintf(stderr, "GLFW error %d: %s\n", e, d); });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(lo.width, lo.height, "RoY Studio Ultimate", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(lo.benchUiTracks > 0 ? 0 : 1);
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    const float dpi = std::max(1.0f, sx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyTheme(dpi);
    loadFonts(dpi);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    if (!app.init(lo.app)) std::fprintf(stderr, "RoY Studio: initialisation reported problems (see log)\n");
    if (!lo.openFile.empty()) app.openProject(lo.openFile);

    int shotArea = 0, shotFrame = 0;
    // --benchui: grow the demo to N tracks, then time 90 frames per area (after 10 warm-up frames)
    std::vector<double> frameMs[static_cast<int>(Area::Count)];
    int benchFrame = 0;
    if (lo.benchUiTracks > 0) {
        if (!app.hasProject()) app.buildDemo(std::filesystem::temp_directory_path() / "RoYStudioBenchUI");
        // Bulk-add tracks inside ONE undo step (macro), then give each new track content.
        std::vector<std::pair<std::string, json>> steps;
        for (int i = static_cast<int>(app.project().tracks.size()); i < lo.benchUiTracks; ++i) {
            const char* type = i % 3 == 0 ? "audio" : i % 3 == 1 ? "midi" : "beat";
            steps.push_back({"AddTrack", {{"type", type}, {"name", std::format("Track {}", i + 1)}}});
        }
        if (!steps.empty() && !app.runMacro("Add tracks", steps)) std::fprintf(stderr, "benchui: %s\n", app.lastError().c_str());
        std::string asset, pattern;
        for (auto& t : app.project().tracks)
            if (!t.audioClips.empty()) asset = t.audioClips[0].assetId;
        if (!app.project().patterns.empty()) pattern = app.project().patterns[0].id;
        steps.clear();
        for (auto& t : app.project().tracks) {
            if (!t.audioClips.empty() || !t.midiClips.empty() || !t.patternClips.empty()) continue;
            if (t.type == TrackType::Audio && !asset.empty())
                steps.push_back({"AddAudioClip", {{"trackId", t.id}, {"assetId", asset}, {"startBeat", 8.0}, {"lengthBeats", 16.0}}});
            else if (t.type == TrackType::Beat && !pattern.empty())
                steps.push_back({"AddPatternClip", {{"trackId", t.id}, {"patternId", pattern}, {"lengthBeats", 32.0}}});
            else if (t.type == TrackType::Midi)
                steps.push_back({"AddMidiClip", {{"trackId", t.id}, {"lengthBeats", 16.0}}});
        }
        if (!steps.empty() && !app.runMacro("Add clips", steps)) std::fprintf(stderr, "benchui: %s\n", app.lastError().c_str());
        app.pixelsPerBeat = 20;
        app.togglePlay(); // meters + playhead moving
    }
    while (!glfwWindowShouldClose(window)) {
        const auto frameStart = std::chrono::steady_clock::now();
        glfwPollEvents();
        app.tick();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (!lo.screenshotDir.empty()) app.area = static_cast<Area>(shotArea);
        if (lo.benchUiTracks > 0) app.area = static_cast<Area>(benchFrame / 100);
        drawStudio(app);
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.043f, 0.043f, 0.055f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!lo.screenshotDir.empty() && ++shotFrame == 6) {
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4), flipped(px.size());
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < h; ++y)
                std::copy(px.begin() + static_cast<long>(y) * w * 4, px.begin() + static_cast<long>(y + 1) * w * 4,
                          flipped.begin() + static_cast<long>(h - 1 - y) * w * 4);
            std::filesystem::create_directories(lo.screenshotDir);
            std::string name = areaName(static_cast<Area>(shotArea));
            for (auto& c : name) if (c == ' ') c = '_';
            const std::string file = lo.screenshotDir + "/" + std::to_string(shotArea + 1) + "_" + name + ".png";
            writePng(file, w, h, flipped);
            std::printf("screenshot %s\n", file.c_str());
            shotFrame = 0;
            if (++shotArea >= static_cast<int>(Area::Count)) glfwSetWindowShouldClose(window, 1);
        }
        glfwSwapBuffers(window);
        if (lo.benchUiTracks > 0) {
            glFinish();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
            if (benchFrame % 100 >= 10) frameMs[benchFrame / 100].push_back(ms);
            if (++benchFrame >= 100 * static_cast<int>(Area::Count)) {
                std::string md = std::format("# RoY Studio UI frame times\n\n{} tracks, window {}x{}, OpenGL renderer: {}\n\n| Area | mean ms | p95 ms | FPS (mean) |\n|---|---|---|---|\n",
                                             app.project().tracks.size(), lo.width, lo.height, reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
                for (int a = 0; a < static_cast<int>(Area::Count); ++a) {
                    auto v = frameMs[a];
                    std::sort(v.begin(), v.end());
                    double sum = 0;
                    for (double x : v) sum += x;
                    const double mean = sum / std::max<size_t>(1, v.size());
                    md += std::format("| {} | {:.1f} | {:.1f} | {:.0f} |\n", areaName(static_cast<Area>(a)), mean, v.empty() ? 0.0 : v[v.size() * 95 / 100], 1000.0 / mean);
                }
                std::printf("%s", md.c_str());
                if (!lo.benchUiOut.empty()) roy::files::atomicWrite(lo.benchUiOut, md);
                glfwSetWindowShouldClose(window, 1);
            }
        }
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
