// RoY Studio - GLFW + OpenGL 3 entry point (Linux/macOS development and CI screenshots).
#include "Options.h"
#include "PngWriter.h"
#include "Ui.h"

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <filesystem>

using namespace roy::gui;

int main(int argc, char** argv) {
    LaunchOptions lo = parseLaunch(argc, argv);
    if ((!lo.screenshotDir.empty() || !lo.selfTestDir.empty()) && lo.app.audioBackend == "auto") lo.app.audioBackend = "null";
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
    glfwSwapInterval(1);
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
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        app.tick();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (!lo.screenshotDir.empty()) app.area = static_cast<Area>(shotArea);
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
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
