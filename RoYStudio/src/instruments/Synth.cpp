#include "instruments/Synth.h"

#include <algorithm>
#include <cmath>

namespace roy {

RoySynth::RoySynth()
    : Instrument({{"osc1Wave", "Osc 1 Wave", 0, 3, 0, "", 4},
                  {"osc2Wave", "Osc 2 Wave", 0, 3, 1, "", 4},
                  {"osc2Detune", "Osc 2 Detune", -100, 100, 7, "cents"},
                  {"osc2Semi", "Osc 2 Semitones", -24, 24, 0, "st", 49},
                  {"oscMix", "Osc Mix", 0, 1, 0.35f},
                  {"attack", "Attack", 0.0005f, 5, 0.005f, "s"},
                  {"decay", "Decay", 0.001f, 5, 0.3f, "s"},
                  {"sustain", "Sustain", 0, 1, 0.7f},
                  {"release", "Release", 0.001f, 10, 0.25f, "s"},
                  {"cutoff", "Cutoff", 20, 20000, 4000, "Hz"},
                  {"resonance", "Resonance", 0.3f, 10, 0.8f},
                  {"filterEnv", "Filter Env", -48, 48, 12, "st"},
                  {"glide", "Glide", 0, 2, 0, "s"},
                  {"mono", "Mono/Legato", 0, 1, 0, "", 2},
                  {"bendRange", "Bend Range", 0, 24, 2, "st", 25},
                  {"volume", "Volume", -60, 12, -9, "dB"}}) {}

void RoySynth::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoySynth::reset() {
    for (auto& v : voices_) {
        v = Voice{};
        v.amp.kill();
        v.filt.kill();
    }
    heldCount_ = 0;
    bend_ = 0;
}

int RoySynth::activeVoices() const {
    int n = 0;
    for (auto& v : voices_) n += v.amp.active() ? 1 : 0;
    return n;
}

void RoySynth::beginBlock(int) noexcept {
    const double sr = sampleRate_;
    for (auto& v : voices_) {
        v.amp.set(sr, p(Attack), p(Decay), p(Sustain), p(Release));
        v.filt.set(sr, p(Attack), p(Decay) * 1.5, 0.2, p(Release));
    }
    const double g = p(Glide);
    glideCoef_ = g <= 0 ? 0.0 : std::exp(-1.0 / (g * 0.25 * sr));
}

double RoySynth::oscillator(int wave, double& phase, double inc, double& tri) noexcept {
    double out = 0;
    switch (wave) {
    case 0: out = 2.0 * phase - 1.0 - dsp::polyBlep(phase, inc); break; // saw
    case 1: {                                                          // square
        out = phase < 0.5 ? 1.0 : -1.0;
        out += dsp::polyBlep(phase, inc);
        out -= dsp::polyBlep(std::fmod(phase + 0.5, 1.0), inc);
        break;
    }
    case 2: { // triangle = integrated band-limited square
        double sq = phase < 0.5 ? 1.0 : -1.0;
        sq += dsp::polyBlep(phase, inc);
        sq -= dsp::polyBlep(std::fmod(phase + 0.5, 1.0), inc);
        tri = inc * sq * 4.0 + (1.0 - 0.0005) * tri;
        out = tri;
        break;
    }
    default: out = std::sin(kTwoPi * phase); break;
    }
    phase += inc;
    if (phase >= 1.0) phase -= 1.0;
    return out;
}

RoySynth::Voice* RoySynth::allocate(int note) noexcept {
    for (auto& v : voices_)
        if (v.note == note && v.amp.active()) return &v; // retrigger same note
    for (auto& v : voices_)
        if (!v.amp.active()) return &v;
    // steal: prefer released voices, then the oldest
    Voice* best = nullptr;
    for (auto& v : voices_)
        if (v.amp.stage() == dsp::Adsr::Stage::Release && (!best || v.age < best->age)) best = &v;
    if (best) return best;
    best = &voices_[0];
    for (auto& v : voices_)
        if (v.age < best->age) best = &v;
    return best;
}

void RoySynth::startVoice(Voice& v, const NoteEvent& e, bool legato) noexcept {
    const bool wasActive = v.amp.active();
    v.note = e.note;
    v.velocity = std::clamp(e.velocity, 0.0f, 1.0f);
    v.targetPitch = e.note + e.detune;
    if (!legato || !wasActive || glideCoef_ == 0) {
        if (!(legato && wasActive)) v.pitch = v.targetPitch;
    }
    if (!(legato && wasActive)) {
        v.amp.noteOn();
        v.filt.noteOn();
        v.phase1 = v.phase2 = 0;
    }
    v.held = true;
    v.age = ++counter_;
}

void RoySynth::handleEvent(const NoteEvent& e) noexcept {
    const bool mono = p(Mono) > 0.5f;
    switch (e.type) {
    case NoteEvent::NoteOn:
        if (e.velocity <= 0) {
            NoteEvent off = e;
            off.type = NoteEvent::NoteOff;
            handleEvent(off);
            return;
        }
        if (mono) {
            if (heldCount_ < 128) heldStack_[heldCount_++] = e.note;
            Voice& v = voices_[0];
            startVoice(v, e, v.held || e.slide);
        } else {
            startVoice(*allocate(e.note), e, false);
        }
        break;
    case NoteEvent::NoteOff:
        if (mono) {
            int w = 0;
            for (int i = 0; i < heldCount_; ++i)
                if (heldStack_[i] != e.note) heldStack_[w++] = heldStack_[i];
            heldCount_ = w;
            Voice& v = voices_[0];
            if (heldCount_ > 0 && v.note == e.note) {
                NoteEvent back = e;
                back.note = static_cast<int16_t>(heldStack_[heldCount_ - 1]);
                back.velocity = v.velocity;
                startVoice(v, back, true);
            } else if (v.note == e.note) {
                v.held = false;
                v.amp.noteOff();
                v.filt.noteOff();
            }
        } else {
            for (auto& v : voices_)
                if (v.note == e.note && v.held) {
                    v.held = false;
                    v.amp.noteOff();
                    v.filt.noteOff();
                }
        }
        break;
    case NoteEvent::AllNotesOff:
        for (auto& v : voices_) {
            v.held = false;
            v.amp.noteOff();
            v.filt.noteOff();
        }
        heldCount_ = 0;
        break;
    case NoteEvent::PitchBend: bend_ = std::clamp(e.value, -1.0f, 1.0f) * p(BendRange); break;
    case NoteEvent::Controller: break;
    }
}

void RoySynth::render(const AudioBlock& io, int start, int end) noexcept {
    const double sr = sampleRate_;
    const int w1 = static_cast<int>(p(Osc1Wave)), w2 = static_cast<int>(p(Osc2Wave));
    const double detune = p(Osc2Detune) / 100.0 + p(Osc2Semi);
    const float mix = p(OscMix);
    const double cutoff = p(Cutoff), res = p(Resonance), fenv = p(FilterEnv);
    const float vol = dbToGain(p(Volume));
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (auto& v : voices_) {
        if (!v.amp.active()) continue;
        for (int i = start; i < end; ++i) {
            if (glideCoef_ > 0) v.pitch = v.targetPitch + (v.pitch - v.targetPitch) * glideCoef_;
            else v.pitch = v.targetPitch;
            const double pitch = v.pitch + bend_;
            const double f1 = midiToHz(pitch), f2 = midiToHz(pitch + detune);
            const double inc1 = std::min(0.45, f1 / sr), inc2 = std::min(0.45, f2 / sr);
            const double o = (1.0 - mix) * oscillator(w1, v.phase1, inc1, v.tri1) + mix * oscillator(w2, v.phase2, inc2, v.tri2);
            const float fe = v.filt.next();
            if ((i & 15) == 0 || i == start) v.filter.set(sr, cutoff * std::pow(2.0, fenv * fe / 12.0), res);
            const float y = v.filter.process(static_cast<float>(o)).lp;
            const float a = v.amp.next();
            const float s = y * a * v.velocity * vol;
            L[i] += s;
            R[i] += s;
            if (!v.amp.active()) {
                v.note = -1;
                break;
            }
        }
    }
}

} // namespace roy
