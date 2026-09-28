// TEST PLUGIN (CLAP): hangs forever in clap_entry.init. Used to test the scan timeout.
#include <clap/clap.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace {
bool eInit(const char*) {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    return true;
}
void eDeinit() {}
const void* eFactory(const char*) { return nullptr; }
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eFactory};
