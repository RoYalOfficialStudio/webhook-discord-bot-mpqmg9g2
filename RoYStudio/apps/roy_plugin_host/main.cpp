// RoYPluginHost - out-of-process plugin scanner and sandbox host.
// Third-party plugin code only ever runs in this process, never inside RoY Studio.
#include "plugins/HostService.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
// A crashing plugin must end this process at once with its exception code, so the
// DAW detects "PLUGIN CRASHED" immediately: no Windows Error Reporting dialog, no
// just-in-time debugger, no waiting.
static LONG WINAPI terminateOnCrash(EXCEPTION_POINTERS* e) {
    const UINT code = e && e->ExceptionRecord ? static_cast<UINT>(e->ExceptionRecord->ExceptionCode) : 0xC0000001u;
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(terminateOnCrash);
#endif
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "--version") {
        std::printf("RoYPluginHost %s\n", ROY_VERSION_STRING);
        return 0;
    }
    if (cmd == "--scan" && argc == 3) return roy::pluginhost::runScan(argv[2]);
    if (cmd == "--host" && argc == 5) return roy::pluginhost::runHost(argv[2], argv[3], argv[4]);
    std::fprintf(stderr,
                 "usage: roy_plugin_host --version\n"
                 "       roy_plugin_host --scan <module.clap>\n"
                 "       roy_plugin_host --host <module.clap> <pluginId> <sharedMemoryName>\n");
    return 2;
}
