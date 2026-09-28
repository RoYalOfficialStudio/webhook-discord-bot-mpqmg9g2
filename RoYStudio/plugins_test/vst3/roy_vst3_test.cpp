// RoY Studio VST3 TEST module (built from the official Steinberg VST3 SDK classes).
// Used only by the test-suite. Classes:
//   "RoY VST3 Gain"  - stereo effect, params Gain (id 0) / Bypass (id 1) / GUI Touch (id 2),
//                      processor + controller state, an editor view (IPlugView) with resize.
//   "RoY VST3 Synth" - instrument, note events -> sine, param Volume (id 0).
//   "RoY VST3 Crash" - effect that dereferences null after 20 process calls.
//   "RoY VST3 Hang"  - effect that stops returning from process() after 20 calls.
#include "base/source/fstreamer.h"

#include <chrono>
#include <thread>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace roytest {

static const FUID kGainProcUID(0x524F5901, 0x47414950, 0x524F4331, 0x00000001);
static const FUID kGainCtrlUID(0x524F5901, 0x47414943, 0x54524C31, 0x00000002);
static const FUID kSynthProcUID(0x524F5902, 0x53594E50, 0x524F4331, 0x00000003);
static const FUID kSynthCtrlUID(0x524F5902, 0x53594E43, 0x54524C31, 0x00000004);
static const FUID kCrashProcUID(0x524F5903, 0x43524150, 0x524F4331, 0x00000005);
static const FUID kHangProcUID(0x524F5904, 0x48414E47, 0x524F4331, 0x00000006);

enum { kGain = 0, kBypass = 1, kGuiTouch = 2 };

// ------------------------------------------------------------------ gain processor
class GainProcessor : public AudioEffect {
public:
    GainProcessor() { setControllerClass(kGainCtrlUID); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new GainProcessor()); }
    tresult PLUGIN_API initialize(FUnknown* ctx) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(ctx);
        if (r != kResultOk) return r;
        addAudioInput(STR16("In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 nIn, SpeakerArrangement* out, int32 nOut) SMTG_OVERRIDE {
        if (nIn == 1 && nOut == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo) return AudioEffect::setBusArrangements(in, nIn, out, nOut);
        return kResultFalse;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 s) SMTG_OVERRIDE { return s == kSample32 ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API process(ProcessData& d) SMTG_OVERRIDE {
        if (d.inputParameterChanges) {
            for (int32 i = 0; i < d.inputParameterChanges->getParameterCount(); ++i) {
                IParamValueQueue* q = d.inputParameterChanges->getParameterData(i);
                ParamValue v;
                int32 off;
                if (!q || q->getPointCount() <= 0 || q->getPoint(q->getPointCount() - 1, off, v) != kResultOk) continue;
                if (q->getParameterId() == kGain) gain_ = v;
                if (q->getParameterId() == kBypass) bypass_ = v > 0.5;
            }
        }
        if (d.numInputs < 1 || d.numOutputs < 1 || d.numSamples <= 0) return kResultOk;
        const float g = bypass_ ? 1.0f : static_cast<float>(gain_ * 2.0);
        for (int32 c = 0; c < 2; ++c) {
            const float* in = d.inputs[0].channelBuffers32[c];
            float* out = d.outputs[0].channelBuffers32[c];
            for (int32 i = 0; i < d.numSamples; ++i) out[i] = in[i] * g;
        }
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        double g;
        int32 b;
        if (!st.readDouble(g) || !st.readInt32(b)) return kResultFalse;
        gain_ = g;
        bypass_ = b != 0;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        st.writeDouble(gain_);
        st.writeInt32(bypass_ ? 1 : 0);
        return kResultOk;
    }

private:
    double gain_ = 0.5; // normalized; plain gain = 2 * normalized
    bool bypass_ = false;
};

// ------------------------------------------------------------------ editor view
class TestView : public CPluginView {
public:
    explicit TestView(EditController* c) : CPluginView(nullptr), controller_(c) {
        ViewRect r(0, 0, 400, 300);
        setRect(r);
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE {
        return (std::strcmp(type, kPlatformTypeHWND) == 0 || std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API canResize() SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) SMTG_OVERRIDE {
        if (r->getWidth() < 200) r->right = r->left + 200;
        if (r->getHeight() < 150) r->bottom = r->top + 150;
        return kResultTrue;
    }
    void attachedToParent() SMTG_OVERRIDE {
        // Ask the host frame for a bigger window (exercises IPlugFrame::resizeView) and
        // simulate a user touching a knob in the GUI (IComponentHandler::performEdit).
        if (plugFrame) {
            ViewRect r(0, 0, 420, 320);
            plugFrame->resizeView(this, &r);
        }
        if (auto* h = controller_->getComponentHandler()) {
            h->beginEdit(kGuiTouch);
            controller_->setParamNormalized(kGuiTouch, 0.75);
            h->performEdit(kGuiTouch, 0.75);
            h->endEdit(kGuiTouch);
        }
    }

private:
    EditController* controller_;
};

// ------------------------------------------------------------------ gain controller
class GainController : public EditController {
public:
    static FUnknown* create(void*) { return static_cast<IEditController*>(new GainController()); }
    tresult PLUGIN_API initialize(FUnknown* ctx) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(ctx);
        if (r != kResultOk) return r;
        parameters.addParameter(STR16("Gain"), STR16("x"), 0, 0.5, ParameterInfo::kCanAutomate, kGain);
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kBypass);
        parameters.addParameter(STR16("GUI Touch"), nullptr, 0, 0, ParameterInfo::kCanAutomate, kGuiTouch);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        double g;
        int32 b;
        if (!st.readDouble(g) || !st.readInt32(b)) return kResultFalse;
        setParamNormalized(kGain, g);
        setParamNormalized(kBypass, b ? 1.0 : 0.0);
        return kResultOk;
    }
    // controller-only state (e.g. UI zoom) - verifies both state chunks round-trip
    tresult PLUGIN_API setState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        int32 z;
        if (!st.readInt32(z)) return kResultFalse;
        uiZoom_ = z;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        st.writeInt32(uiZoom_);
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if (name && std::strcmp(name, ViewType::kEditor) == 0) {
            uiZoom_ += 1; // opening the editor changes controller state
            return new TestView(this);
        }
        return nullptr;
    }

private:
    int32 uiZoom_ = 100;
};

// ------------------------------------------------------------------ synth
class SynthProcessor : public AudioEffect {
public:
    SynthProcessor() { setControllerClass(kSynthCtrlUID); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new SynthProcessor()); }
    tresult PLUGIN_API initialize(FUnknown* ctx) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(ctx);
        if (r != kResultOk) return r;
        addEventInput(STR16("Events"), 16);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup& s) SMTG_OVERRIDE {
        sr_ = s.sampleRate;
        return AudioEffect::setupProcessing(s);
    }
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        phase_ = 0; // (de)activation resets the processing state, as the VST3 spec expects
        note_ = -1;
        return AudioEffect::setActive(state);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 s) SMTG_OVERRIDE { return s == kSample32 ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API process(ProcessData& d) SMTG_OVERRIDE {
        if (d.inputParameterChanges)
            for (int32 i = 0; i < d.inputParameterChanges->getParameterCount(); ++i) {
                IParamValueQueue* q = d.inputParameterChanges->getParameterData(i);
                ParamValue v;
                int32 off;
                if (q && q->getParameterId() == 0 && q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, off, v) == kResultOk) volume_ = v;
            }
        if (d.numOutputs < 1) return kResultOk;
        const int32 nEv = d.inputEvents ? d.inputEvents->getEventCount() : 0;
        int32 ev = 0;
        float* L = d.outputs[0].channelBuffers32[0];
        float* R = d.outputs[0].channelBuffers32[1];
        for (int32 i = 0; i < d.numSamples; ++i) {
            while (ev < nEv) {
                Event e{};
                d.inputEvents->getEvent(ev, e);
                if (e.sampleOffset > i) break;
                if (e.type == Event::kNoteOnEvent) note_ = e.noteOn.pitch;
                else if (e.type == Event::kNoteOffEvent && e.noteOff.pitch == note_) note_ = -1;
                ++ev;
            }
            float v = 0.0f;
            if (note_ >= 0) {
                v = static_cast<float>(0.25 * volume_ * 2.0 * std::sin(6.283185307179586 * phase_));
                phase_ += 440.0 * std::pow(2.0, (note_ - 69) / 12.0) / sr_;
                if (phase_ > 1.0) phase_ -= 1.0;
            }
            L[i] = R[i] = v;
        }
        d.outputs[0].silenceFlags = note_ < 0 ? 3 : 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        double v;
        if (!st.readDouble(v)) return kResultFalse;
        volume_ = v;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        st.writeDouble(volume_);
        return kResultOk;
    }

private:
    double sr_ = 48000, phase_ = 0, volume_ = 0.5;
    int note_ = -1;
};

class SynthController : public EditController {
public:
    static FUnknown* create(void*) { return static_cast<IEditController*>(new SynthController()); }
    tresult PLUGIN_API initialize(FUnknown* ctx) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(ctx);
        if (r != kResultOk) return r;
        parameters.addParameter(STR16("Volume"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, 0);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* s) SMTG_OVERRIDE {
        IBStreamer st(s, kLittleEndian);
        double v;
        if (!st.readDouble(v)) return kResultFalse;
        setParamNormalized(0, v);
        return kResultOk;
    }
};

// ------------------------------------------------------------------ crash
class CrashProcessor : public GainProcessor {
public:
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new CrashProcessor()); }
    tresult PLUGIN_API process(ProcessData& d) SMTG_OVERRIDE {
        if (++blocks_ > 20) {
            volatile int* bad = nullptr;
            *bad = 1;
            std::abort();
        }
        return GainProcessor::process(d);
    }

private:
    int blocks_ = 0;
};

// ------------------------------------------------------------------ hang
class HangProcessor : public GainProcessor {
public:
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new HangProcessor()); }
    tresult PLUGIN_API process(ProcessData& d) SMTG_OVERRIDE {
        if (++blocks_ > 20)
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        return GainProcessor::process(d);
    }

private:
    int blocks_ = 0;
};

} // namespace roytest

BEGIN_FACTORY_DEF("RoY Studio (test)", "https://example.invalid/roy-test", "mailto:test@example.invalid")
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kGainProcUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "RoY VST3 Gain",
           Vst::kDistributable, "Fx|Tools", "1.3.0", kVstVersionString, roytest::GainProcessor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kGainCtrlUID), PClassInfo::kManyInstances, kVstComponentControllerClass, "RoY VST3 Gain Controller",
           0, "", "1.3.0", kVstVersionString, roytest::GainController::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kSynthProcUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "RoY VST3 Synth",
           Vst::kDistributable, "Instrument|Synth", "1.0.0", kVstVersionString, roytest::SynthProcessor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kSynthCtrlUID), PClassInfo::kManyInstances, kVstComponentControllerClass, "RoY VST3 Synth Controller",
           0, "", "1.0.0", kVstVersionString, roytest::SynthController::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kCrashProcUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "RoY VST3 Crash",
           Vst::kDistributable, "Fx", "0.1.0", kVstVersionString, roytest::CrashProcessor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(roytest::kHangProcUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "RoY VST3 Hang",
           Vst::kDistributable, "Fx", "0.1.0", kVstVersionString, roytest::HangProcessor::create)
END_FACTORY
