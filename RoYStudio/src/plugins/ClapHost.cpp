#include "plugins/ClapHost.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace roy::clap {

namespace {
void* openLibrary(const std::string& path, std::string* error) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    HMODULE h = LoadLibraryW(w.c_str());
    if (!h && error) *error = std::format("LoadLibrary failed ({})", GetLastError());
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && error) *error = std::string("dlopen failed: ") + dlerror();
    return h;
#endif
}
void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}
void closeLibrary(void* lib) {
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}
} // namespace

// ------------------------------------------------------------------ Module
Module::~Module() {
    if (initialised_ && entry_) entry_->deinit();
    if (lib_) closeLibrary(lib_);
}

std::unique_ptr<Module> Module::load(const std::string& path, std::string* error) {
    std::unique_ptr<Module> m(new Module());
    m->path_ = path;
    m->lib_ = openLibrary(path, error);
    if (!m->lib_) return nullptr;
    m->entry_ = static_cast<const clap_plugin_entry_t*>(symbol(m->lib_, "clap_entry"));
    if (!m->entry_) {
        if (error) *error = "no clap_entry symbol";
        return nullptr;
    }
    if (!clap_version_is_compatible(m->entry_->clap_version)) {
        if (error)
            *error = std::format("incompatible CLAP version {}.{}.{}", m->entry_->clap_version.major, m->entry_->clap_version.minor,
                                 m->entry_->clap_version.revision);
        return nullptr;
    }
    if (!m->entry_->init(path.c_str())) {
        if (error) *error = "clap_entry.init returned false";
        return nullptr;
    }
    m->initialised_ = true;
    m->factory_ = static_cast<const clap_plugin_factory_t*>(m->entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!m->factory_) {
        if (error) *error = "module has no plugin factory";
        return nullptr;
    }
    return m;
}

bool isInstrument(const clap_plugin_descriptor_t* d) {
    if (!d->features) return false;
    for (const char* const* f = d->features; *f; ++f)
        if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT)) return true;
    return false;
}

json describe(const clap_plugin_descriptor_t* d) {
    auto s = [](const char* c) { return std::string(c ? c : ""); };
    json features = json::array();
    if (d->features)
        for (const char* const* f = d->features; *f; ++f) features.push_back(*f);
    return {{"id", s(d->id)}, {"name", s(d->name)}, {"vendor", s(d->vendor)}, {"version", s(d->version)},
            {"description", s(d->description)}, {"features", features}, {"instrument", isInstrument(d)},
            {"clapVersion", std::format("{}.{}.{}", d->clap_version.major, d->clap_version.minor, d->clap_version.revision)}};
}

json Module::describeAll() const {
    json out = json::array();
    const uint32_t n = factory_->get_plugin_count(factory_);
    for (uint32_t i = 0; i < n; ++i)
        if (auto* d = factory_->get_plugin_descriptor(factory_, i)) out.push_back(describe(d));
    return out;
}

// ------------------------------------------------------------------ Instance
const void* Instance::hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void Instance::hostRequestRestart(const clap_host_t*) {}
void Instance::hostRequestProcess(const clap_host_t*) {}
void Instance::hostRequestCallback(const clap_host_t* h) {
    static_cast<Instance*>(h->host_data)->callbackRequested_.store(true);
}

Instance::~Instance() {
    if (!plugin_) return;
    if (active_) deactivate();
    plugin_->destroy(plugin_);
}

std::unique_ptr<Instance> Instance::create(Module& m, const std::string& pluginId, std::string* error) {
    std::unique_ptr<Instance> inst(new Instance());
    inst->host_.clap_version = CLAP_VERSION;
    inst->host_.host_data = inst.get();
    inst->host_.name = "RoY Studio";
    inst->host_.vendor = "RoY Studio";
    inst->host_.url = "";
    inst->host_.version = ROY_VERSION_STRING;
    inst->host_.get_extension = &Instance::hostGetExtension;
    inst->host_.request_restart = &Instance::hostRequestRestart;
    inst->host_.request_process = &Instance::hostRequestProcess;
    inst->host_.request_callback = &Instance::hostRequestCallback;
    const clap_plugin_t* p = m.factory()->create_plugin(m.factory(), &inst->host_, pluginId.c_str());
    if (!p) {
        if (error) *error = std::format("factory could not create '{}'", pluginId);
        return nullptr;
    }
    if (!p->init(p)) {
        p->destroy(p);
        if (error) *error = "plugin init failed";
        return nullptr;
    }
    inst->plugin_ = p;
    inst->instrument_ = isInstrument(p->desc);
    inst->paramsExt_ = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
    inst->stateExt_ = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    inst->latencyExt_ = static_cast<const clap_plugin_latency_t*>(p->get_extension(p, CLAP_EXT_LATENCY));
    if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS))) {
        clap_audio_port_info_t info{};
        inst->inChannels_ = ports->count(p, true) > 0 && ports->get(p, 0, true, &info) ? static_cast<int>(info.channel_count) : 0;
        inst->outChannels_ = ports->count(p, false) > 0 && ports->get(p, 0, false, &info) ? static_cast<int>(info.channel_count) : 0;
    } else {
        inst->inChannels_ = 0; // no ports extension: no audio I/O declared
        inst->outChannels_ = 0;
    }
    if (inst->paramsExt_) {
        const uint32_t n = inst->paramsExt_->count(p);
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t pi{};
            if (!inst->paramsExt_->get_info(p, i, &pi)) continue;
            inst->params_.push_back({pi.id, pi.name, pi.min_value, pi.max_value, pi.default_value, pi.flags});
        }
    }
    return inst;
}

json Instance::info() const {
    json j = describe(plugin_->desc);
    json ps = json::array();
    for (auto& p : params_)
        ps.push_back({{"id", p.id}, {"name", p.name}, {"min", p.minValue}, {"max", p.maxValue}, {"default", p.defaultValue},
                      {"value", paramValue(p.id)}, {"stepped", (p.flags & CLAP_PARAM_IS_STEPPED) != 0},
                      {"hidden", (p.flags & CLAP_PARAM_IS_HIDDEN) != 0}});
    j["params"] = ps;
    j["inputChannels"] = inChannels_;
    j["outputChannels"] = outChannels_;
    j["hasState"] = stateExt_ != nullptr;
    return j;
}

bool Instance::activate(double sampleRate, uint32_t maxFrames) {
    if (active_) deactivate();
    active_ = plugin_->activate(plugin_, sampleRate, 1, maxFrames);
    return active_;
}

void Instance::deactivate() {
    if (!active_) return;
    // start/stop processing belong to the audio thread; the host guarantees it is idle here.
    stopProcessingIfStarted();
    plugin_->deactivate(plugin_);
    active_ = false;
}

void Instance::reset() {
    if (active_) plugin_->reset(plugin_);
}

uint32_t Instance::latency() const { return latencyExt_ && active_ ? latencyExt_->get(plugin_) : 0; }

double Instance::paramValue(clap_id id) const {
    double v = 0;
    if (paramsExt_ && paramsExt_->get_value(plugin_, id, &v)) return v;
    return 0;
}

namespace {
struct OStream {
    clap_ostream_t s;
    std::vector<uint8_t>* out;
};
int64_t oWrite(const clap_ostream_t* s, const void* buf, uint64_t size) {
    auto* o = static_cast<OStream*>(s->ctx);
    const auto* b = static_cast<const uint8_t*>(buf);
    o->out->insert(o->out->end(), b, b + size);
    return static_cast<int64_t>(size);
}
struct IStream {
    clap_istream_t s;
    const std::vector<uint8_t>* in;
    size_t pos;
};
int64_t iRead(const clap_istream_t* s, void* buf, uint64_t size) {
    auto* i = static_cast<IStream*>(s->ctx);
    const size_t n = std::min<size_t>(static_cast<size_t>(size), i->in->size() - i->pos);
    if (n) std::memcpy(buf, i->in->data() + i->pos, n);
    i->pos += n;
    return static_cast<int64_t>(n);
}
} // namespace

bool Instance::saveState(std::vector<uint8_t>& out) {
    out.clear();
    if (!stateExt_) return false;
    OStream o{{nullptr, oWrite}, &out};
    o.s.ctx = &o;
    return stateExt_->save(plugin_, &o.s);
}

bool Instance::loadState(const std::vector<uint8_t>& in) {
    if (!stateExt_) return false;
    IStream i{{nullptr, iRead}, &in, 0};
    i.s.ctx = &i;
    return stateExt_->load(plugin_, &i.s);
}

void Instance::idle() {
    if (callbackRequested_.exchange(false)) plugin_->on_main_thread(plugin_);
}

namespace {
bool dropEvent(const clap_output_events_t*, const clap_event_header_t*) { return true; }
} // namespace

int32_t Instance::process(float* const* in, float* const* out, uint32_t frames, const clap_input_events_t* events,
                          const clap_event_transport_t* transport) noexcept {
    if (!active_) return CLAP_PROCESS_ERROR;
    if (!processing_) processing_ = plugin_->start_processing(plugin_);
    if (!processing_) return CLAP_PROCESS_ERROR;
    clap_audio_buffer_t inBuf{};
    clap_audio_buffer_t outBuf{};
    float* inPtrs[2] = {in ? in[0] : nullptr, in ? in[1] : nullptr};
    float* outPtrs[2] = {out[0], out[1]};
    inBuf.data32 = inPtrs;
    inBuf.channel_count = static_cast<uint32_t>(std::min(2, std::max(inChannels_, 0)));
    outBuf.data32 = outPtrs;
    outBuf.channel_count = static_cast<uint32_t>(std::min(2, std::max(outChannels_, 0)));
    clap_output_events_t outEv{nullptr, &dropEvent}; // output events (param gestures) are not used yet
    clap_process_t proc{};
    proc.steady_time = steady_;
    proc.frames_count = frames;
    proc.transport = transport;
    proc.audio_inputs = inChannels_ > 0 && in ? &inBuf : nullptr;
    proc.audio_inputs_count = inChannels_ > 0 && in ? 1 : 0;
    proc.audio_outputs = outChannels_ > 0 ? &outBuf : nullptr;
    proc.audio_outputs_count = outChannels_ > 0 ? 1 : 0;
    proc.in_events = events;
    proc.out_events = &outEv;
    steady_ += frames;
    return plugin_->process(plugin_, &proc);
}

void Instance::stopProcessingIfStarted() noexcept {
    if (processing_) plugin_->stop_processing(plugin_);
    processing_ = false;
}

} // namespace roy::clap
