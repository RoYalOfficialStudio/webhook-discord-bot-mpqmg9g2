// roy_plugin_host - out-of-process plugin scanner/host (sandbox).
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "--version") {
        std::printf("RoYPluginHost %s\n", ROY_VERSION_STRING);
        return 0;
    }
    std::fprintf(stderr, "usage: roy_plugin_host --version\n");
    return 2;
}
