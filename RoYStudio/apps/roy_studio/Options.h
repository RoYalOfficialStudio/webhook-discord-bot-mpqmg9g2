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
    int benchUiTracks = 0;     // --benchui <tracks>: measure frame times per area with a large project
    std::string benchUiOut;    // --benchui-out <file.md>
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
        else if (a == "--benchui" && i + 1 < argc) o.benchUiTracks = std::atoi(argv[++i]);
        else if (a == "--benchui-out" && i + 1 < argc) o.benchUiOut = argv[++i];
        else if (a == "--first-run") o.app.forceFirstRun = true;
        else if (a == "--size" && i + 2 < argc) {
            o.width = std::atoi(argv[++i]);
            o.height = std::atoi(argv[++i]);
        } else if (!a.empty() && a[0] != '-') o.openFile = a;
    }
    // automated runs never touch the user's settings.json
    if (!o.screenshotDir.empty() || !o.selfTestDir.empty() || o.benchUiTracks > 0) o.app.interactive = false;
    return o;
}

} // namespace roy::gui
