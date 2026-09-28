#include "pluginhost/ClapHost.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace roy::clap {

namespace pipc = roy::pluginipc;

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

// ------------------------------------------------------------------ events (preallocated)
union AnyEvent {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_param_value_t param;
    clap_event_midi_t midi;
};

struct Instance::EventStore {
    clap_input_events_t in{};
    clap_output_events_t out{};
    AnyEvent events[pipc::kMaxEvents + pipc::kMaxParamChanges + 256];
    uint32_t count = 0;
    pipc::ParamChange outParams[pipc::kMaxParamChanges];
    uint32_t numOut = 0;
    EventStore() {
        in.ctx = this;
        in.size = [](const clap_input_events_t* l) { return static_cast<const EventStore*>(l->ctx)->count; };
        in.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
            auto* self = static_cast<const EventStore*>(l->ctx);
            return i < self->count ? &self->events[i].header : nullptr;
        };
        out.ctx = this;
        out.try_push = [](const clap_output_events_t* l, const clap_event_header_t* e) -> bool {
            auto* self = static_cast<EventStore*>(l->ctx);
            if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE && self->numOut < pipc::kMaxParamChanges) {
                auto* p = reinterpret_cast<const clap_event_param_value_t*>(e);
                self->outParams[self->numOut++] = {e->time, p->param_id, p->value};
            }
            return true; // gestures etc. are accepted and ignored
        };
    }
    void addParam(uint32_t time, uint32_t id, double value) {
        AnyEvent& e = events[count++];
        e.param = {};
        e.param.header = {sizeof(clap_event_param_value_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param.param_id = id;
        e.param.note_id = -1;
        e.param.port_index = -1;
        e.param.channel = -1;
        e.param.key = -1;
        e.param.value = value;
    }
    void addNote(const pipc::Event& n) {
        AnyEvent& e = events[count++];
        switch (n.type) {
        case pipc::EvNoteOn:
        case pipc::EvNoteOff:
            e.note = {};
            e.note.header = {sizeof(clap_event_note_t), n.offset, CLAP_CORE_EVENT_SPACE_ID,
                             static_cast<uint16_t>(n.type == pipc::EvNoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note.note_id = -1;
            e.note.port_index = 0;
            e.note.channel = n.channel;
            e.note.key = n.note;
            e.note.velocity = n.velocity;
            break;
        case pipc::EvAllNotesOff:
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xB0 | (n.channel & 15));
            e.midi.data[1] = 123;
            break;
        case pipc::EvPitchBend: {
            const int v = std::clamp(static_cast<int>(std::lround((n.value + 1.0f) * 8191.5f)), 0, 16383);
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xE0 | (n.channel & 15));
            e.midi.data[1] = static_cast<uint8_t>(v & 127);
            e.midi.data[2] = static_cast<uint8_t>(v >> 7);
            break;
        }
        default:
            e.midi = {};
            e.midi.header = {sizeof(clap_event_midi_t), n.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.midi.data[0] = static_cast<uint8_t>(0xB0 | (n.channel & 15));
            e.midi.data[1] = static_cast<uint8_t>(n.controller & 127);
            e.midi.data[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(n.value * 127.0f + 0.5f), 0, 127));
            break;
        }
    }
};

// ------------------------------------------------------------------ host extensions
struct HostExt {
    static Instance* self(const clap_host_t* h) { return static_cast<Instance*>(h->host_data); }
    // gui
    static void resizeHintsChanged(const clap_host_t*) {}
    static bool requestResize(const clap_host_t* h, uint32_t w, uint32_t hh) {
        auto* s = self(h);
        if (!s->window_.isOpen()) return false;
        s->window_.resize(static_cast<int>(w), static_cast<int>(hh));
        return true;
    }
    static bool requestShow(const clap_host_t* h) { return self(h)->window_.isOpen(); }
    static bool requestHide(const clap_host_t* h) { return self(h)->window_.isOpen(); }
    static void closed(const clap_host_t* h, bool wasDestroyed) { self(h)->closeRequested_ = true; }
    // timers
    static bool registerTimer(const clap_host_t* h, uint32_t periodMs, clap_id* timerId) {
        auto* s = self(h);
        static clap_id next = 1;
        const clap_id id = next++;
        const int rl = s->runLoop_.addTimer(static_cast<int>(periodMs), [s, id] {
            if (s->timerExt_) s->timerExt_->on_timer(s->plugin_, id);
        });
        s->timerIds_.push_back({id, rl});
        *timerId = id;
        return true;
    }
    static bool unregisterTimer(const clap_host_t* h, clap_id timerId) {
        auto* s = self(h);
        for (size_t i = 0; i < s->timerIds_.size(); ++i)
            if (s->timerIds_[i].first == timerId) {
                s->runLoop_.removeTimer(s->timerIds_[i].second);
                s->timerIds_.erase(s->timerIds_.begin() + static_cast<long>(i));
                return true;
            }
        return false;
    }
    // posix fds
    static bool registerFd(const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto* s = self(h);
        const int rl = s->runLoop_.addFd(fd, [s, fd] {
            if (s->fdExt_) s->fdExt_->on_fd(s->plugin_, fd, CLAP_POSIX_FD_READ);
        });
        s->fdIds_.push_back({fd, rl});
        return true;
    }
    static bool modifyFd(const clap_host_t*, int, clap_posix_fd_flags_t) { return true; }
    static bool unregisterFd(const clap_host_t* h, int fd) {
        auto* s = self(h);
        for (size_t i = 0; i < s->fdIds_.size(); ++i)
            if (s->fdIds_[i].first == fd) {
                s->runLoop_.removeFd(s->fdIds_[i].second);
                s->fdIds_.erase(s->fdIds_.begin() + static_cast<long>(i));
                return true;
            }
        return false;
    }
    // params / latency / log
    static void paramsRescan(const clap_host_t*, clap_param_rescan_flags) {}
    static void paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void paramsRequestFlush(const clap_host_t* h) { self(h)->callbackRequested_ = true; }
    static void latencyChanged(const clap_host_t* h) { self(h)->restartRequested_ = true; }
    static void logMsg(const clap_host_t*, clap_log_severity sev, const char* msg) {
        log::write(sev >= CLAP_LOG_ERROR ? log::Level::Error : sev == CLAP_LOG_WARNING ? log::Level::Warn : log::Level::Debug, "plugin", msg ? msg : "");
    }
};

namespace {
const clap_host_gui_t kHostGui = {HostExt::resizeHintsChanged, HostExt::requestResize, HostExt::requestShow, HostExt::requestHide, HostExt::closed};
const clap_host_timer_support_t kHostTimer = {HostExt::registerTimer, HostExt::unregisterTimer};
const clap_host_posix_fd_support_t kHostFd = {HostExt::registerFd, HostExt::modifyFd, HostExt::unregisterFd};
const clap_host_params_t kHostParams = {HostExt::paramsRescan, HostExt::paramsClear, HostExt::paramsRequestFlush};
const clap_host_latency_t kHostLatency = {HostExt::latencyChanged};
const clap_host_log_t kHostLog = {HostExt::logMsg};
} // namespace

const void* Instance::hostGetExtension(const clap_host_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kHostGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kHostFd;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kHostLatency;
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kHostLog;
    return nullptr;
}
void Instance::hostRequestRestart(const clap_host_t* h) { static_cast<Instance*>(h->host_data)->restartRequested_ = true; }
void Instance::hostRequestProcess(const clap_host_t*) {}
void Instance::hostRequestCallback(const clap_host_t* h) { static_cast<Instance*>(h->host_data)->callbackRequested_.store(true); }

// ------------------------------------------------------------------ Instance
Instance::~Instance() { shutdown(); }

void Instance::shutdown() {
    if (!plugin_) return;
    closeEditor();
    if (active_) deactivate();
    plugin_->destroy(plugin_);
    plugin_ = nullptr;
}

std::unique_ptr<Instance> Instance::create(Module& m, const std::string& pluginId, std::string* error) {
    std::unique_ptr<Instance> inst(new Instance());
    inst->events_ = std::make_unique<EventStore>();
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
    inst->guiExt_ = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
    inst->timerExt_ = static_cast<const clap_plugin_timer_support_t*>(p->get_extension(p, CLAP_EXT_TIMER_SUPPORT));
    inst->fdExt_ = static_cast<const clap_plugin_posix_fd_support_t*>(p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT));
    if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS))) {
        clap_audio_port_info_t info{};
        inst->inChannels_ = ports->count(p, true) > 0 && ports->get(p, 0, true, &info) ? static_cast<int>(info.channel_count) : 0;
        inst->outChannels_ = ports->count(p, false) > 0 && ports->get(p, 0, false, &info) ? static_cast<int>(info.channel_count) : 0;
    } else {
        inst->inChannels_ = 0;
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

json Instance::info() {
    json j = describe(plugin_->desc);
    j["format"] = "clap";
    json ps = json::array();
    for (auto& p : params_) {
        const bool stepped = (p.flags & CLAP_PARAM_IS_STEPPED) != 0;
        ps.push_back({{"id", p.id}, {"name", p.name}, {"min", p.minValue}, {"max", p.maxValue}, {"default", p.defaultValue},
                      {"value", paramValue(p.id)}, {"stepped", stepped}, {"steps", stepped ? static_cast<int>(p.maxValue - p.minValue) + 1 : 0},
                      {"hidden", (p.flags & CLAP_PARAM_IS_HIDDEN) != 0}, {"readOnly", (p.flags & CLAP_PARAM_IS_READONLY) != 0},
                      {"bypass", (p.flags & CLAP_PARAM_IS_BYPASS) != 0}});
    }
    j["params"] = ps;
    j["inputChannels"] = inChannels_;
    j["outputChannels"] = outChannels_;
    j["hasState"] = stateExt_ != nullptr;
    j["hasEditor"] = editorSupported();
    return j;
}

bool Instance::activate(double sampleRate, uint32_t maxFrames) {
    if (active_) deactivate();
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    active_ = plugin_->activate(plugin_, sampleRate, 1, maxFrames);
    return active_;
}

void Instance::deactivate() {
    if (!active_) return;
    if (processing_) plugin_->stop_processing(plugin_);
    processing_ = false;
    plugin_->deactivate(plugin_);
    active_ = false;
}

uint32_t Instance::latency() { return latencyExt_ && active_ ? latencyExt_->get(plugin_) : 0; }

void Instance::reset() {
    if (active_) plugin_->reset(plugin_);
}

double Instance::paramValue(clap_id id) const {
    double v = 0;
    if (paramsExt_ && paramsExt_->get_value(plugin_, id, &v)) return v;
    return 0;
}

json Instance::paramValues() {
    json values = json::object();
    for (auto& p : params_) values[std::to_string(p.id)] = paramValue(p.id);
    return values;
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
    runLoop_.run();
    if (window_.isOpen()) window_.pump();
    if (closeRequested_) {
        closeRequested_ = false;
        closeEditor();
    }
}

int32_t Instance::process(pipc::Block& b) noexcept {
    if (!active_) return CLAP_PROCESS_ERROR;
    if (!processing_) processing_ = plugin_->start_processing(plugin_);
    if (!processing_) return CLAP_PROCESS_ERROR;
    EventStore& es = *events_;
    // input events: parameter changes (RoY + plugin GUI) and notes, merged in time order
    es.count = 0;
    es.numOut = 0;
    Edit gui[64];
    const int ng = takeGuiEdits(gui, 64);
    for (int i = 0; i < ng; ++i) es.addParam(0, gui[i].id, gui[i].value);
    const uint32_t np = std::min(b.numParamChanges, pipc::kMaxParamChanges);
    const uint32_t ne = std::min(b.numEvents, pipc::kMaxEvents);
    uint32_t i = 0, k = 0;
    while (i < np || k < ne) {
        const bool takeParam = k >= ne || (i < np && b.params[i].offset <= b.events[k].offset);
        if (takeParam) {
            es.addParam(b.params[i].offset, b.params[i].paramId, b.params[i].value);
            ++i;
        } else {
            es.addNote(b.events[k++]);
        }
    }
    clap_event_transport_t transport{};
    transport.header = {sizeof(clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    transport.flags = static_cast<uint32_t>(CLAP_TRANSPORT_HAS_TEMPO) | ((b.transportFlags & 1u) ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
    transport.tempo = b.tempo;
    const uint32_t frames = std::min(b.frames, pipc::kMaxFrames);
    float* inPtrs[2] = {b.in[0], b.in[1]};
    float* outPtrs[2] = {b.out[0], b.out[1]};
    clap_audio_buffer_t inBuf{};
    clap_audio_buffer_t outBuf{};
    inBuf.data32 = inPtrs;
    inBuf.channel_count = static_cast<uint32_t>(std::min(2, std::max(inChannels_, 0)));
    outBuf.data32 = outPtrs;
    outBuf.channel_count = static_cast<uint32_t>(std::min(2, std::max(outChannels_, 0)));
    clap_process_t proc{};
    proc.steady_time = steady_;
    proc.frames_count = frames;
    proc.transport = &transport;
    proc.audio_inputs = inChannels_ > 0 ? &inBuf : nullptr;
    proc.audio_inputs_count = inChannels_ > 0 ? 1 : 0;
    proc.audio_outputs = outChannels_ > 0 ? &outBuf : nullptr;
    proc.audio_outputs_count = outChannels_ > 0 ? 1 : 0;
    proc.in_events = &es.in;
    proc.out_events = &es.out;
    steady_ += frames;
    const int32_t st = plugin_->process(plugin_, &proc);
    if (outChannels_ == 1) std::memcpy(b.out[1], b.out[0], sizeof(float) * frames);
    // plugin -> RoY parameter changes: GUI edits first, then plugin output events
    uint32_t n = 0;
    for (int g = 0; g < ng && n < pipc::kMaxParamChanges; ++g) b.outParams[n++] = {0, gui[g].id, gui[g].value};
    for (uint32_t o = 0; o < es.numOut && n < pipc::kMaxParamChanges; ++o) b.outParams[n++] = es.outParams[o];
    b.numOutParams = n;
    return st;
}

// ------------------------------------------------------------------ editor
bool Instance::editorSupported() {
    if (!guiExt_) return false;
#ifdef _WIN32
    return guiExt_->is_api_supported(plugin_, CLAP_WINDOW_API_WIN32, false);
#else
    return guiExt_->is_api_supported(plugin_, CLAP_WINDOW_API_X11, false);
#endif
}

bool Instance::openEditor(bool alwaysOnTop, std::string* error) {
    if (window_.isOpen()) {
        window_.setAlwaysOnTop(alwaysOnTop);
        window_.focus();
        return true;
    }
    if (!editorSupported()) {
        if (error) *error = "plugin has no embeddable editor for this platform";
        return false;
    }
#ifdef _WIN32
    const char* api = CLAP_WINDOW_API_WIN32;
#else
    const char* api = CLAP_WINDOW_API_X11;
#endif
    if (!guiExt_->create(plugin_, api, false)) {
        if (error) *error = "clap gui.create failed";
        return false;
    }
    guiCreated_ = true;
    uint32_t w = 400, h = 300;
    guiExt_->get_size(plugin_, &w, &h);
    const bool resizable = guiExt_->can_resize(plugin_);
    if (!window_.create(plugin_->desc->name ? plugin_->desc->name : "Plugin", static_cast<int>(w), static_cast<int>(h), resizable, alwaysOnTop, error)) {
        guiExt_->destroy(plugin_);
        guiCreated_ = false;
        return false;
    }
    guiExt_->set_scale(plugin_, window_.dpiScale());
    window_.onCloseRequested = [this] { closeRequested_ = true; };
    window_.onUserResized = [this](int nw, int nh) {
        uint32_t aw = static_cast<uint32_t>(nw), ah = static_cast<uint32_t>(nh);
        if (guiExt_->can_resize(plugin_) && guiExt_->adjust_size(plugin_, &aw, &ah)) guiExt_->set_size(plugin_, aw, ah);
    };
    clap_window_t win{};
    win.api = api;
#ifdef _WIN32
    win.win32 = window_.nativeHandle();
#else
    win.x11 = static_cast<clap_xwnd>(reinterpret_cast<uintptr_t>(window_.nativeHandle()));
#endif
    if (!guiExt_->set_parent(plugin_, &win)) {
        closeEditor();
        if (error) *error = "clap gui.set_parent failed";
        return false;
    }
    guiExt_->show(plugin_);
    return true;
}

void Instance::closeEditor() {
    // Closing the window never destroys the plugin instance - only its GUI.
    if (guiCreated_ && guiExt_) {
        guiExt_->hide(plugin_);
        guiExt_->destroy(plugin_);
    }
    guiCreated_ = false;
    window_.destroy();
}

json Instance::editorState() {
    return {{"open", window_.isOpen()}, {"width", window_.width()}, {"height", window_.height()}, {"supported", editorSupported()},
            {"api", window_.isOpen() ? window_.platformType() : ""}};
}

} // namespace roy::clap
