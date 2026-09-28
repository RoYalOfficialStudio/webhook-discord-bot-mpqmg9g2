// TEST PLUGIN (CLAP): deliberately crashes. Used to test sandboxing / quarantine.
//  - ROY_TEST_CRASH_AT_SCAN=1 in the environment: crashes inside clap_entry.init (scan crash)
//  - otherwise: loads fine, passes audio through and crashes after 20 processed blocks
#include <clap/clap.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace {
const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT, "com.roystudio.test.crash", "RoY Test Crash", "RoY Studio (test)", "", "", "",
                                         "0.1.0", "Crashes on purpose", kFeatures};
struct Plug {
    clap_plugin_t plugin;
    int blocks = 0;
};
Plug* self(const clap_plugin_t* p) { return static_cast<Plug*>(p->plugin_data); }

[[noreturn]] void crash() {
    volatile int* bad = nullptr;
    *bad = 42; // segmentation fault / access violation
    std::abort();
}

uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }
bool portsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (index) return false;
    info->id = isInput ? 0 : 1;
    std::strcpy(info->name, "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

bool pInit(const clap_plugin_t*) { return true; }
void pDestroy(const clap_plugin_t* p) { delete self(p); }
bool pActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pDeactivate(const clap_plugin_t*) {}
bool pStart(const clap_plugin_t*) { return true; }
void pStop(const clap_plugin_t*) {}
void pReset(const clap_plugin_t*) {}
clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
    if (++self(p)->blocks > 20) crash();
    for (uint32_t c = 0; c < 2; ++c)
        std::memcpy(proc->audio_outputs[0].data32[c], proc->audio_inputs[0].data32[c], sizeof(float) * proc->frames_count);
    return CLAP_PROCESS_CONTINUE;
}
const void* pExt(const clap_plugin_t*, const char* id) { return !std::strcmp(id, CLAP_EXT_AUDIO_PORTS) ? &kPorts : nullptr; }
void pMain(const clap_plugin_t*) {}

uint32_t fCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    if (std::strcmp(id, kDesc.id)) return nullptr;
    Plug* s = new (std::nothrow) Plug{};
    if (!s) return nullptr;
    s->plugin = {&kDesc, s, pInit, pDestroy, pActivate, pDeactivate, pStart, pStop, pReset, pProcess, pExt, pMain};
    return &s->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

bool eInit(const char*) {
    const char* env = std::getenv("ROY_TEST_CRASH_AT_SCAN");
    if (env && env[0] == '1') crash();
    return true;
}
void eDeinit() {}
const void* eFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eFactory};
