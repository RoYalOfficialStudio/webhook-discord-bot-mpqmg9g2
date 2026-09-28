#pragma once
// Command-line options shared by both platform entry points.
#include "App.h"

#include <cstring>
#include <string>

namespace roy::gui {

struct LaunchOptions {
    AppOptions app;
    std::string screenshotDir; // --screenshots <dir>: render every area once, save PNGs, exit
    std::string openFile;      // project file to open
    std::string selfTestDir;   // --selftest <dir>: run App::selfTest and exit
    int width = 1600, height = 900;
};

inline LaunchOptions parseLaunch(int argc, char** argv) {
    LaunchOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--demo") o.app.demo = true;
        else if (a == "--demo-folder" && i + 1 < argc) o.app.demoFolder = argv[++i];
        else if (a == "--audio" && i + 1 < argc) o.app.audioBackend = argv[++i];
        else if (a == "--screenshots" && i + 1 < argc) o.screenshotDir = argv[++i];
        else if (a == "--selftest" && i + 1 < argc) o.selfTestDir = argv[++i];
        else if (a == "--size" && i + 2 < argc) {
            o.width = std::atoi(argv[++i]);
            o.height = std::atoi(argv[++i]);
        } else if (!a.empty() && a[0] != '-') o.openFile = a;
    }
    return o;
}

} // namespace roy::gui
