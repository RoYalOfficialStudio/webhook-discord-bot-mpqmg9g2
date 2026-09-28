#include "pluginhost/Vst3Host.h"
#include "core/Log.h"
#include "pluginhost/EditorWindow.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace roy::vst3 {

namespace pipc = roy::pluginipc;

namespace {
std::string utf8(const TChar* s) { return StringConvert::convert(std::u16string(reinterpret_cast<const char16_t*>(s))); }

// One plugin instance per RoYPluginHost process: its GUI timers / file descriptors live in one
// process-wide run loop, pumped by Instance::idle() on the main thread.
pluginhost::RunLoop& hostRunLoop() {
    static pluginhost::RunLoop loop;
    return loop;
}

#ifndef _WIN32
// Linux::IRunLoop on top of the host run loop. Since VST SDK 3.7 VSTGUI takes its run loop from the
// HOST CONTEXT (IPluginFactory3::setHostContext), older plugins from the IPlugFrame - RoY offers both.
class RunLoopAdapter {
public:
    tresult registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) {
        if (!h) return kInvalidArgument;
        const int id = hostRunLoop().addFd(fd, [h, fd] { h->onFDIsSet(fd); });
        fds_.push_back({h, id});
        return kResultTrue;
    }
    tresult unregisterEventHandler(Linux::IEventHandler* h) {
        for (auto it = fds_.begin(); it != fds_.end();)
            if (it->first == h) {
                hostRunLoop().removeFd(it->second);
                it = fds_.erase(it);
            } else {
                ++it;
            }
        return kResultTrue;
    }
    tresult registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) {
        if (!h) return kInvalidArgument;
        const int id = hostRunLoop().addTimer(static_cast<int>(ms), [h] { h->onTimer(); });
        timers_.push_back({h, id});
        return kResultTrue;
    }
    tresult unregisterTimer(Linux::ITimerHandler* h) {
        for (auto it = timers_.begin(); it != timers_.end();)
            if (it->first == h) {
                hostRunLoop().removeTimer(it->second);
                it = timers_.erase(it);
            } else {
                ++it;
            }
        return kResultTrue;
    }
    size_t registrations() const { return fds_.size() + timers_.size(); }

private:
    std::vector<std::pair<Linux::IEventHandler*, int>> fds_;
    std::vector<std::pair<Linux::ITimerHandler*, int>> timers_;
};

class RoyHostApplication : public HostApplication, public Linux::IRunLoop {
public:
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) override { return loop_.registerEventHandler(h, fd); }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override { return loop_.unregisterEventHandler(h); }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) override { return loop_.registerTimer(h, ms); }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override { return loop_.unregisterTimer(h); }
    RunLoopAdapter loop_;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) {
            HostApplication::addRef();
            *obj = static_cast<Linux::IRunLoop*>(this);
            return kResultOk;
        }
        return HostApplication::queryInterface(iid, obj);
    }
    uint32 PLUGIN_API addRef() override { return HostApplication::addRef(); }
    uint32 PLUGIN_API release() override { return HostApplication::release(); }
};
#else
using RoyHostApplication = HostApplication;
#endif

HostApplication& hostContext() {
    static RoyHostApplication* h = [] {
        auto* app = new RoyHostApplication();
        PluginContextFactory::instance().setPluginContext(static_cast<HostApplication*>(app));
        return app;
    }();
    return *h;
}

bool isInstrument(const VST3::Hosting::ClassInfo& ci) { return ci.subCategoriesString().find("Instrument") != std::string::npos; }
} // namespace

// ------------------------------------------------------------------ instance
class Instance final : public pluginhost::HostedPlugin {
public:
    ~Instance() override { shutdown(); }

    bool init(const std::string& path, const std::string& classId, std::string* error) {
        std::string err;
        module_ = VST3::Hosting::Module::create(path, err);
        if (!module_) {
            if (error) *error = "cannot load VST3 module: " + err;
            return false;
        }
        const auto& factory = module_->getFactory();
        factory.setHostContext(&hostContext()); // IPluginFactory3: VSTGUI (Linux) gets the run loop from here
        for (auto& ci : factory.classInfos())
            if (ci.category() == kVstAudioEffectClass && (classId.empty() || ci.ID().toString() == classId)) {
                info_ = ci;
                found_ = true;
                break;
            }
        if (!found_) {
            if (error) *error = "class " + classId + " not found in module";
            return false;
        }
        component_ = factory.createInstance<IComponent>(info_.ID());
        if (!component_ || component_->initialize(&hostContext()) != kResultOk) {
            if (error) *error = "IComponent could not be created/initialized";
            component_ = nullptr;
            return false;
        }
        componentInitialized_ = true;
        processor_ = U::cast<IAudioProcessor>(component_);
        if (!processor_) {
            if (error) *error = "component has no IAudioProcessor";
            return false;
        }
        controller_ = U::cast<IEditController>(component_);
        if (!controller_) {
            TUID ccid;
            if (component_->getControllerClassId(ccid) == kResultTrue) {
                controller_ = factory.createInstance<IEditController>(VST3::UID::fromTUID(ccid));
                if (controller_ && controller_->initialize(&hostContext()) == kResultOk) separateController_ = true;
                else controller_ = nullptr;
            }
        }
        if (controller_) {
            controller_->setComponentHandler(&handler_);
            if (separateController_) {
                auto compCP = U::cast<IConnectionPoint>(component_);
                auto ctrlCP = U::cast<IConnectionPoint>(controller_);
                if (compCP && ctrlCP) {
                    compProxy_ = owned(new ConnectionProxy(compCP));
                    ctrlProxy_ = owned(new ConnectionProxy(ctrlCP));
                    compProxy_->connect(ctrlCP);
                    ctrlProxy_->connect(compCP);
                }
            }
            // controller learns the component's default state
            MemoryStream s;
            if (component_->getState(&s) == kResultOk) {
                s.seek(0, IBStream::kIBSeekSet, nullptr);
                controller_->setComponentState(&s);
            }
            const int32 n = controller_->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                ParameterInfo pi{};
                if (controller_->getParameterInfo(i, pi) == kResultOk) params_.push_back(pi);
            }
            if (auto mm = U::cast<IMidiMapping>(controller_))
                for (int cc = 0; cc < 130; ++cc) {
                    ParamID id;
                    if (mm->getMidiControllerAssignment(0, 0, static_cast<CtrlNumber>(cc), id) == kResultTrue) ccMap_[static_cast<size_t>(cc)] = static_cast<int64_t>(id);
                }
        }
        inBuses_ = component_->getBusCount(kAudio, kInput);
        outBuses_ = component_->getBusCount(kAudio, kOutput);
        eventBuses_ = component_->getBusCount(kEvent, kInput);
        return true;
    }

    json info() override {
        json ps = json::array();
        for (auto& p : params_)
            ps.push_back({{"id", p.id}, {"name", utf8(p.title)}, {"min", 0.0}, {"max", 1.0}, {"default", p.defaultNormalizedValue},
                          {"value", controller_ ? controller_->getParamNormalized(p.id) : p.defaultNormalizedValue}, {"stepped", p.stepCount > 0},
                          {"steps", p.stepCount > 0 ? p.stepCount + 1 : 0}, {"hidden", (p.flags & ParameterInfo::kIsHidden) != 0},
                          {"readOnly", (p.flags & ParameterInfo::kIsReadOnly) != 0}, {"bypass", (p.flags & ParameterInfo::kIsBypass) != 0},
                          {"units", utf8(p.units)}});
        return {{"format", "vst3"}, {"id", info_.ID().toString()}, {"name", info_.name()}, {"vendor", info_.vendor()},
                {"version", info_.version()}, {"sdkVersion", info_.sdkVersion()}, {"subCategories", info_.subCategoriesString()},
                {"instrument", isInstrument(info_)}, {"params", ps}, {"inputChannels", inBuses_ > 0 ? 2 : 0},
                {"outputChannels", outBuses_ > 0 ? 2 : 0}, {"hasState", true}, {"hasEditor", editorSupported()},
                {"separateController", separateController_}, {"eventInputs", eventBuses_}};
    }

    bool activate(double sampleRate, uint32_t maxFrames) override {
        deactivate();
        sampleRate_ = sampleRate;
        maxFrames_ = static_cast<int32>(maxFrames);
        SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
        processor_->setBusArrangements(inBuses_ > 0 ? &in : nullptr, inBuses_ > 0 ? 1 : 0, outBuses_ > 0 ? &out : nullptr, outBuses_ > 0 ? 1 : 0);
        if (inBuses_ > 0) component_->activateBus(kAudio, kInput, 0, true);
        if (outBuses_ > 0) component_->activateBus(kAudio, kOutput, 0, true);
        if (eventBuses_ > 0) component_->activateBus(kEvent, kInput, 0, true);
        ProcessSetup setup{kRealtime, kSample32, maxFrames_, sampleRate};
        if (processor_->setupProcessing(setup) != kResultOk) return false;
        if (component_->setActive(true) != kResultOk) return false;
        active_ = true;
        data_.prepare(*component_, 0, kSample32); // buffers are pointed at shared memory per block
        inChanges_.setMaxParameters(static_cast<int32>(params_.size()) + 64);
        outChanges_.setMaxParameters(static_cast<int32>(params_.size()) + 64);
        events_.setMaxSize(static_cast<int32>(pipc::kMaxEvents) + 256);
        return true;
    }

    void deactivate() {
        if (!active_) return;
        if (processing_) processor_->setProcessing(false);
        processing_ = false;
        component_->setActive(false);
        data_.unprepare();
        active_ = false;
    }

    uint32_t latency() override { return active_ ? processor_->getLatencySamples() : 0; }

    void reset() override {
        // VST3 has no reset(): a deactivate/activate cycle clears tails and voices.
        if (!active_) return;
        if (processing_) processor_->setProcessing(false);
        processing_ = false;
        component_->setActive(false);
        component_->setActive(true);
    }

    bool saveState(std::vector<uint8_t>& out) override {
        MemoryStream cs, ks;
        if (component_->getState(&cs) != kResultOk) return false;
        if (separateController_ && controller_) controller_->getState(&ks);
        auto put64 = [&](uint64_t v) {
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        out.clear();
        out.insert(out.end(), {'R', 'V', '3', 'S', 1, 0, 0, 0});
        put64(static_cast<uint64_t>(cs.getSize()));
        out.insert(out.end(), cs.getData(), cs.getData() + cs.getSize());
        put64(static_cast<uint64_t>(ks.getSize()));
        if (ks.getSize() > 0) out.insert(out.end(), ks.getData(), ks.getData() + ks.getSize());
        return true;
    }

    bool loadState(const std::vector<uint8_t>& in) override {
        if (in.size() < 24 || std::memcmp(in.data(), "RV3S", 4) != 0) return false;
        auto get64 = [&](size_t at) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[at + static_cast<size_t>(i)]) << (8 * i);
            return v;
        };
        const uint64_t cl = get64(8);
        if (16 + cl + 8 > in.size()) return false;
        const uint64_t kl = get64(16 + cl);
        if (24 + cl + kl > in.size()) return false;
        MemoryStream cs(const_cast<uint8_t*>(in.data()) + 16, static_cast<TSize>(cl));
        // A plugin without state (getState -> kNotImplemented, empty chunk) is not a rejection.
        if (cl > 0) {
            const tresult r = component_->setState(&cs);
            if (r != kResultOk && r != kNotImplemented) return false;
        }
        if (controller_ && cl > 0) {
            cs.seek(0, IBStream::kIBSeekSet, nullptr);
            controller_->setComponentState(&cs);
            if (kl > 0 && separateController_) {
                MemoryStream ks(const_cast<uint8_t*>(in.data()) + 24 + cl, static_cast<TSize>(kl));
                controller_->setState(&ks);
            }
        }
        return true;
    }

    json paramValues() override {
        json v = json::object();
        for (auto& p : params_) v[std::to_string(p.id)] = controller_ ? controller_->getParamNormalized(p.id) : 0.0;
        return v;
    }

    int32_t process(pipc::Block& b) noexcept override {
        if (!active_) return 0; // 0 = error (CLAP_PROCESS_ERROR semantics)
        if (!processing_) {
            processor_->setProcessing(true);
            processing_ = true;
        }
        inChanges_.clearQueue();
        outChanges_.clearQueue();
        events_.clear();
        auto addChange = [&](ParamID id, int32 offset, double value) {
            int32 idx = 0;
            if (IParamValueQueue* q = inChanges_.addParameterData(id, idx)) {
                int32 pi = 0;
                q->addPoint(offset, value, pi);
            }
        };
        Edit gui[64];
        const int ng = takeGuiEdits(gui, 64);
        for (int i = 0; i < ng; ++i) addChange(gui[i].id, 0, gui[i].value);
        for (uint32_t i = 0; i < std::min(b.numParamChanges, pipc::kMaxParamChanges); ++i)
            addChange(b.params[i].paramId, static_cast<int32>(b.params[i].offset), b.params[i].value);
        for (uint32_t k = 0; k < std::min(b.numEvents, pipc::kMaxEvents); ++k) {
            const pipc::Event& n = b.events[k];
            Event e{};
            e.busIndex = 0;
            e.sampleOffset = static_cast<int32>(n.offset);
            switch (n.type) {
            case pipc::EvNoteOn:
                e.type = Event::kNoteOnEvent;
                e.noteOn = {static_cast<int16>(n.channel), n.note, 0.0f, n.velocity, 0, -1};
                notesOn_[static_cast<size_t>(n.note & 127)] = true;
                events_.addEvent(e);
                break;
            case pipc::EvNoteOff:
                e.type = Event::kNoteOffEvent;
                e.noteOff = {static_cast<int16>(n.channel), n.note, 0.0f, -1, 0.0f};
                notesOn_[static_cast<size_t>(n.note & 127)] = false;
                events_.addEvent(e);
                break;
            case pipc::EvAllNotesOff:
                for (int16 p = 0; p < 128; ++p)
                    if (notesOn_[static_cast<size_t>(p)]) {
                        e.type = Event::kNoteOffEvent;
                        e.noteOff = {static_cast<int16>(n.channel), p, 0.0f, -1, 0.0f};
                        events_.addEvent(e);
                        notesOn_[static_cast<size_t>(p)] = false;
                    }
                break;
            case pipc::EvPitchBend:
                if (ccMap_[kPitchBend] >= 0) addChange(static_cast<ParamID>(ccMap_[kPitchBend]), e.sampleOffset, (n.value + 1.0) * 0.5);
                break;
            default:
                if (n.controller >= 0 && n.controller < 128 && ccMap_[static_cast<size_t>(n.controller)] >= 0)
                    addChange(static_cast<ParamID>(ccMap_[static_cast<size_t>(n.controller)]), e.sampleOffset, n.value);
                break;
            }
        }
        ctx_ = {};
        ctx_.state = ProcessContext::kTempoValid | ((b.transportFlags & 1u) ? ProcessContext::kPlaying : 0) | ProcessContext::kContTimeValid;
        ctx_.sampleRate = sampleRate_;
        ctx_.tempo = b.tempo;
        ctx_.projectTimeSamples = b.songPosSamples;
        ctx_.continousTimeSamples = steady_;
        const int32 frames = static_cast<int32>(std::min(b.frames, pipc::kMaxFrames));
        float* inPtrs[2] = {b.in[0], b.in[1]};
        float* outPtrs[2] = {b.out[0], b.out[1]};
        if (inBuses_ > 0) data_.setChannelBuffers(kInput, 0, inPtrs, 2);
        if (outBuses_ > 0) data_.setChannelBuffers(kOutput, 0, outPtrs, 2);
        data_.numSamples = frames;
        data_.inputParameterChanges = &inChanges_;
        data_.outputParameterChanges = &outChanges_;
        data_.inputEvents = eventBuses_ > 0 ? &events_ : nullptr;
        data_.processContext = &ctx_;
        const tresult r = processor_->process(data_);
        steady_ += frames;
        // plugin -> RoY: GUI edits + output parameter changes (last value per parameter)
        uint32_t n = 0;
        for (int g = 0; g < ng && n < pipc::kMaxParamChanges; ++g) b.outParams[n++] = {0, gui[g].id, gui[g].value};
        for (int32 i = 0; i < outChanges_.getParameterCount() && n < pipc::kMaxParamChanges; ++i) {
            IParamValueQueue* q = outChanges_.getParameterData(i);
            int32 off;
            ParamValue v;
            if (q && q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, off, v) == kResultOk) b.outParams[n++] = {static_cast<uint32_t>(off), q->getParameterId(), v};
        }
        b.numOutParams = n;
        // keep the controller (and its GUI) in sync with what the processor received
        {
            std::unique_lock<std::mutex> lk(syncMutex_, std::try_to_lock);
            if (lk.owns_lock()) {
                for (uint32_t i = 0; i < std::min(b.numParamChanges, pipc::kMaxParamChanges) && syncCount_ < syncBuf_.size(); ++i)
                    syncBuf_[syncCount_++] = {b.params[i].paramId, b.params[i].value};
                for (uint32_t i = 0; i < n && syncCount_ < syncBuf_.size(); ++i) syncBuf_[syncCount_++] = {b.outParams[i].paramId, b.outParams[i].value};
            }
        }
        return r == kResultOk ? 1 : 0;
    }

    void idle() override {
        {
            std::lock_guard<std::mutex> lk(syncMutex_);
            if (controller_)
                for (size_t i = 0; i < syncCount_; ++i) controller_->setParamNormalized(syncBuf_[i].id, syncBuf_[i].value);
            syncCount_ = 0;
        }
        hostRunLoop().run();
        if (window_.isOpen()) window_.pump();
        if (closeRequested_) {
            closeRequested_ = false;
            closeEditor();
        }
    }

    bool editorSupported() override { return controller_ != nullptr; }

    bool openEditor(bool alwaysOnTop, std::string* error) override {
        if (window_.isOpen()) {
            window_.setAlwaysOnTop(alwaysOnTop);
            window_.focus();
            return true;
        }
        if (!controller_) {
            if (error) *error = "plugin has no edit controller";
            return false;
        }
        view_ = owned(controller_->createView(ViewType::kEditor));
        if (!view_) {
            if (error) *error = "plugin provides no editor view";
            return false;
        }
#ifdef _WIN32
        const FIDString type = kPlatformTypeHWND;
#else
        const FIDString type = kPlatformTypeX11EmbedWindowID;
#endif
        if (view_->isPlatformTypeSupported(type) != kResultTrue) {
            view_ = nullptr;
            if (error) *error = std::string("editor does not support ") + type;
            return false;
        }
        ViewRect r(0, 0, 400, 300);
        view_->getSize(&r);
        const bool resizable = view_->canResize() == kResultTrue;
        if (!window_.create(info_.name(), r.getWidth(), r.getHeight(), resizable, alwaysOnTop, error)) {
            view_ = nullptr;
            return false;
        }
        window_.onCloseRequested = [this] { closeRequested_ = true; };
        window_.onUserResized = [this](int w, int h) {
            if (!view_) return;
            ViewRect nr(0, 0, w, h);
            if (view_->canResize() == kResultTrue) {
                view_->checkSizeConstraint(&nr);
                view_->onSize(&nr);
            }
        };
        view_->setFrame(&frame_);
        if (auto cs = U::cast<IPlugViewContentScaleSupport>(view_)) cs->setContentScaleFactor(static_cast<float>(window_.dpiScale()));
        if (view_->attached(window_.nativeHandle(), type) != kResultOk) {
            closeEditor();
            if (error) *error = "IPlugView::attached failed";
            return false;
        }
        attached_ = true;
        return true;
    }

    void closeEditor() override {
        // Closing the editor never destroys the plugin instance - only its view.
        if (view_) {
            if (attached_) view_->removed();
            view_->setFrame(nullptr);
            view_ = nullptr;
        }
        attached_ = false;
        window_.destroy();
    }

    json editorState() override {
        return {{"open", window_.isOpen()}, {"width", window_.width()}, {"height", window_.height()}, {"supported", editorSupported()},
                {"api", window_.isOpen() ? window_.platformType() : ""}, {"resizeRequests", resizeRequests_}, {"timers", hostRunLoop().timerCount()}};
    }

    void shutdown() override {
        if (!component_) {
            module_.reset();
            return;
        }
        closeEditor();
        deactivate();
        if (compProxy_) compProxy_->disconnect(U::cast<IConnectionPoint>(controller_));
        if (ctrlProxy_) ctrlProxy_->disconnect(U::cast<IConnectionPoint>(component_));
        compProxy_ = nullptr;
        ctrlProxy_ = nullptr;
        if (controller_) controller_->setComponentHandler(nullptr);
        if (separateController_ && controller_) controller_->terminate();
        controller_ = nullptr;
        processor_ = nullptr;
        if (componentInitialized_) component_->terminate();
        component_ = nullptr;
        module_.reset(); // unload
    }

    void onGuiEdit(ParamID id, ParamValue v) { pushGuiEdit(id, v); }

    // ---- host-side COM objects -------------------------------------------------
    class Handler : public IComponentHandler {
    public:
        Instance* owner = nullptr;
        tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
        tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
            owner->onGuiEdit(id, v);
            return kResultOk;
        }
        tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
        tresult PLUGIN_API restartComponent(int32 flags) override {
            owner->restartFlags_ |= flags;
            return kResultOk;
        }
        tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
            QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponentHandler)
            QUERY_INTERFACE(iid, obj, IComponentHandler::iid, IComponentHandler)
            *obj = nullptr;
            return kNoInterface;
        }
        uint32 PLUGIN_API addRef() override { return 1000; }
        uint32 PLUGIN_API release() override { return 1000; }
    };

    class Frame : public IPlugFrame
#ifndef _WIN32
        , public Linux::IRunLoop
#endif
    {
    public:
        Instance* owner = nullptr;
        tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
            if (!r || !owner->window_.isOpen()) return kResultFalse;
            ++owner->resizeRequests_;
            owner->window_.resize(r->getWidth(), r->getHeight());
            if (view) view->onSize(r);
            return kResultTrue;
        }
#ifndef _WIN32
        tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) override { return loop_.registerEventHandler(h, fd); }
        tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override { return loop_.unregisterEventHandler(h); }
        tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) override { return loop_.registerTimer(h, ms); }
        tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override { return loop_.unregisterTimer(h); }
        RunLoopAdapter loop_;
#endif
        tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
            QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
            QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
#ifndef _WIN32
            QUERY_INTERFACE(iid, obj, Linux::IRunLoop::iid, Linux::IRunLoop)
#endif
            *obj = nullptr;
            return kNoInterface;
        }
        uint32 PLUGIN_API addRef() override { return 1000; }
        uint32 PLUGIN_API release() override { return 1000; }
    };

    Instance() {
        handler_.owner = this;
        frame_.owner = this;
        ccMap_.fill(-1);
    }

private:
    VST3::Hosting::Module::Ptr module_;
    VST3::Hosting::ClassInfo info_;
    bool found_ = false;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    IPtr<ConnectionProxy> compProxy_, ctrlProxy_;
    bool separateController_ = false, componentInitialized_ = false;
    std::vector<ParameterInfo> params_;
    std::array<int64_t, 130> ccMap_{};
    int32 inBuses_ = 0, outBuses_ = 0, eventBuses_ = 0;
    bool active_ = false, processing_ = false;
    double sampleRate_ = 48000;
    int32 maxFrames_ = 512;
    int64_t steady_ = 0;
    HostProcessData data_;
    ParameterChanges inChanges_, outChanges_;
    EventList events_;
    ProcessContext ctx_{};
    std::array<bool, 128> notesOn_{};
    struct Sync { ParamID id; double value; };
    std::mutex syncMutex_;
    std::array<Sync, 1024> syncBuf_{};
    size_t syncCount_ = 0;
    int32 restartFlags_ = 0;
    // editor
    Handler handler_;
    Frame frame_;
    IPtr<IPlugView> view_;
    bool attached_ = false;
    bool closeRequested_ = false;
    int resizeRequests_ = 0;
    pluginhost::EditorWindow window_;
};

std::unique_ptr<pluginhost::HostedPlugin> createInstance(const std::string& path, const std::string& classId, std::string* error) {
    auto inst = std::make_unique<Instance>();
    if (!inst->init(path, classId, error)) return nullptr;
    return inst;
}

json scanModule(const std::string& path) {
    std::string err;
    auto module = VST3::Hosting::Module::create(path, err);
    if (!module) return {{"ok", false}, {"path", path}, {"error", "cannot load VST3 module: " + err}};
    const auto& factory = module->getFactory();
    const std::string factoryVendor = factory.info().vendor();
    json plugins = json::array();
    for (auto& ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        json p = {{"id", ci.ID().toString()}, {"name", ci.name()}, {"vendor", ci.vendor().empty() ? factoryVendor : ci.vendor()},
                  {"version", ci.version()}, {"sdkVersion", ci.sdkVersion()}, {"instrument", isInstrument(ci)}};
        json features = json::array();
        for (auto& s : ci.subCategories()) features.push_back(s);
        p["features"] = features;
        std::string ierr;
        auto inst = createInstance(path, ci.ID().toString(), &ierr);
        p["instantiates"] = inst != nullptr;
        if (inst) {
            const json info = inst->info();
            p["paramCount"] = info["params"].size();
            p["inputChannels"] = info["inputChannels"];
            p["outputChannels"] = info["outputChannels"];
            p["hasEditor"] = info["hasEditor"];
            inst->shutdown();
        } else {
            p["error"] = ierr;
        }
        plugins.push_back(p);
    }
    return {{"ok", true}, {"path", path}, {"format", "vst3"}, {"plugins", plugins}};
}

} // namespace roy::vst3
