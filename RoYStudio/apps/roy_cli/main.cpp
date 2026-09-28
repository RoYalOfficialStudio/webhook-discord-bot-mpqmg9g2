// roy_cli - headless RoY Studio command line (grows with each GMB).
#include "audio/DeviceManager.h"
#include "core/Log.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "help";
    if (cmd == "--version" || cmd == "version") {
        std::printf("RoY Studio %s\n", ROY_VERSION_STRING);
        return 0;
    }
    if (cmd == "devices") {
        roy::DeviceManager dm;
        std::string err;
        if (!dm.initialise(argc > 2 ? argv[2] : "auto", &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("backend: %s\n", dm.backendName().c_str());
        for (auto& d : dm.outputDevices()) std::printf("  out %s%s\n", d.name.c_str(), d.isDefault ? " (default)" : "");
        for (auto& d : dm.inputDevices()) std::printf("  in  %s%s\n", d.name.c_str(), d.isDefault ? " (default)" : "");
        return 0;
    }
    std::printf("usage: roy_cli version|devices [backend]\n");
    return cmd == "help" ? 0 : 1;
}
