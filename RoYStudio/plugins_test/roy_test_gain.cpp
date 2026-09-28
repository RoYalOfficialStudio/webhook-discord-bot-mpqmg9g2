// TEST PLUGIN (CLAP) used by the RoY Studio test-suite.
// Contains two plugins:
//   com.roystudio.test.gain  - stereo gain effect with one parameter + state
//   com.roystudio.test.sine  - monophonic sine instrument (note events)
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

constexpr const char* kGainId = "com.roystudio.test.gain";
constexpr const char* kSineId = "com.roystudio.test.sine";
const char* kGainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
const char* kSineFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

const clap_plugin_descriptor_t kGainDesc = {CLAP_VERSION_INIT, kGainId, "RoY Test Gain", "RoY Studio (test)", "", "", "",
                                             "1.2.0", "Test gain effect", kGainFeatures};
const clap_plugin_descriptor_t kSineDesc = {CLAP_VERSION_INIT, kSineId, "RoY Test Sine", "RoY Studio (test)", "", "", "",
                                             "1.0.0", "Test sine instrument", kSineFeatures};

struct Plug {
    clap_plugin_t plugin;
    const clap_host_t* host;
    bool instrument;
    double sr = 48000;
    double gain = 1.0;   // param 0
    double phase = 0;
    int note = -1;
};

Plug* self(const clap_plugin_t* p) { return static_cast<Plug*>(p->plugin_data); }

// ---- audio ports -------------------------------------------------------------
uint32_t portsCount(const clap_plugin_t* p, bool isInput) { return (isInput && self(p)->instrument) ? 0 : 1; }
bool portsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id = isInput ? 0 : 1;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

// ---- note ports (instrument) ----------------------------------------------------
uint32_t notePortsCount(const clap_plugin_t* p, bool isInput) { return isInput && self(p)->instrument ? 1 : 0; }
bool notePortsGet(const clap_plugin_t*, uint32_t index, bool, clap_note_port_info_t* info) {
    if (index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}
const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

// ---- params ---------------------------------------------------------------------
uint32_t paramsCount(const clap_plugin_t*) { return 1; }
bool paramsInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 0;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "Gain");
    info->min_value = 0.0;
    info->max_value = 2.0;
    info->default_value = 1.0;
    return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) {
    if (id != 0) return false;
    *v = self(p)->gain;
    return true;
}
bool paramsToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
    std::snprintf(out, size, "%.3f", v);
    return true;
}
bool paramsFromText(const clap_plugin_t*, clap_id, const char* t, double* v) {
    *v = std::atof(t);
    return true;
}
void applyEvents(Plug* s, const clap_input_events_t* in) {
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (e->param_id == 0) s->gain = e->value;
        }
    }
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { applyEvents(self(p), in); }
const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

// ---- state ----------------------------------------------------------------------
bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
    const double g = self(p)->gain;
    return s->write(s, &g, sizeof(g)) == static_cast<int64_t>(sizeof(g));
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
    double g = 1.0;
    if (s->read(s, &g, sizeof(g)) != static_cast<int64_t>(sizeof(g))) return false;
    self(p)->gain = g;
    return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---- latency ---------------------------------------------------------------------
uint32_t latencyGet(const clap_plugin_t*) { return 0; }
const clap_plugin_latency_t kLatency = {latencyGet};

// ---- plugin ----------------------------------------------------------------------
bool pInit(const clap_plugin_t*) { return true; }
void pDestroy(const clap_plugin_t* p) { delete self(p); }
bool pActivate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) {
    self(p)->sr = sr;
    return true;
}
void pDeactivate(const clap_plugin_t*) {}
bool pStart(const clap_plugin_t*) { return true; }
void pStop(const clap_plugin_t*) {}
void pReset(const clap_plugin_t* p) { self(p)->phase = 0; }
clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* proc) {
    Plug* s = self(p);
    const uint32_t n = proc->frames_count;
    const uint32_t evCount = proc->in_events->size(proc->in_events);
    uint32_t ev = 0;
    float** out = proc->audio_outputs[0].data32;
    for (uint32_t i = 0; i < n; ++i) {
        while (ev < evCount) {
            const clap_event_header_t* h = proc->in_events->get(proc->in_events, ev);
            if (h->time > i) break;
            if (h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
                if (h->type == CLAP_EVENT_PARAM_VALUE) {
                    auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
                    if (e->param_id == 0) s->gain = e->value;
                } else if (h->type == CLAP_EVENT_NOTE_ON) {
                    s->note = reinterpret_cast<const clap_event_note_t*>(h)->key;
                } else if (h->type == CLAP_EVENT_NOTE_OFF) {
                    if (reinterpret_cast<const clap_event_note_t*>(h)->key == s->note) s->note = -1;
                }
            }
            ++ev;
        }
        if (s->instrument) {
            float v = 0.0f;
            if (s->note >= 0) {
                const double hz = 440.0 * std::pow(2.0, (s->note - 69) / 12.0);
                v = static_cast<float>(0.25 * s->gain * std::sin(2.0 * 3.14159265358979 * s->phase));
                s->phase += hz / s->sr;
                if (s->phase > 1.0) s->phase -= 1.0;
            }
            out[0][i] = v;
            out[1][i] = v;
        } else {
            float** in = proc->audio_inputs[0].data32;
            out[0][i] = static_cast<float>(in[0][i] * s->gain);
            out[1][i] = static_cast<float>(in[1][i] * s->gain);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}
const void* pExt(const clap_plugin_t* p, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS) && self(p)->instrument) return &kNotePorts;
    return nullptr;
}
void pMain(const clap_plugin_t*) {}

// ---- factory -----------------------------------------------------------------------
uint32_t fCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kGainDesc : i == 1 ? &kSineDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    const bool sine = !std::strcmp(id, kSineId);
    if (!sine && std::strcmp(id, kGainId)) return nullptr;
    Plug* s = new (std::nothrow) Plug{};
    if (!s) return nullptr;
    s->host = host;
    s->instrument = sine;
    s->plugin = {sine ? &kSineDesc : &kGainDesc, s, pInit, pDestroy, pActivate, pDeactivate, pStart, pStop, pReset, pProcess, pExt, pMain};
    return &s->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

bool eInit(const char*) { return true; }
void eDeinit() {}
const void* eFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eFactory};
