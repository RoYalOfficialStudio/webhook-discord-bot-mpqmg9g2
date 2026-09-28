#include "instruments/Bass808.h"

#include <algorithm>
#include <cmath>

namespace roy {

Bass808::Bass808()
    : Instrument({{"tune", "Tune", -24, 24, 0, "st"},
                  {"pitchEnv", "Pitch Env", 0, 24, 7, "st"},
                  {"pitchEnvDecay", "Pitch Env Decay", 1, 500, 40, "ms"},
                  {"glide", "Glide", 0, 1000, 80, "ms"},
                  {"attack", "Attack", 0, 200, 1, "ms"},
                  {"hold", "Hold", 0, 2000, 0, "ms"},
                  {"decay", "Decay", 10, 5000, 900, "ms"},
                  {"sustain", "Sustain", 0, 1, 0.0f},
                  {"release", "Release", 5, 3000, 120, "ms"},
                  {"saturation", "Saturation", 0, 1, 0.25f},
                  {"distortion", "Distortion", 0, 1, 0.0f},
                  {"softClip", "Soft Clip", 0, 1, 1, "", 2},
                  {"subLevel", "Sub Level", 0, 1, 1.0f},
                  {"tone", "Tone", 100, 20000, 6000, "Hz"},
                  {"legato", "Legato", 0, 1, 1, "", 2},
                  {"keyLock", "808 Key Lock", 0, 1, 0, "", 2},
                  {"keyRoot", "Key Root", 0, 11, 0, "", 12},
                  {"keyScale", "Key Scale", 0, 12, 2, "", 13},
                  {"volume", "Volume", -60, 12, -6, "dB"},
                  {"startPhase", "Start Phase", 0, 360, 0, "deg"},
                  {"phaseReset", "Phase Reset", 0, 1, 1, "", 2}}) {}

void Bass808::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    dc_.set(sr, 12.0);
    reset();
}

void Bass808::reset() {
    env_.kill();
    tone_.reset();
    dc_.reset();
    phase_ = 0;
    lastOut_ = 0;
    declickPending_ = false;
    declickLeft_ = 0;
    gate_ = false;
    heldCount_ = 0;
    sinceTrigger_ = 1e9;
}

void Bass808::beginBlock(int) noexcept {
    const double sr = sampleRate_;
    env_.set(sr, p(Attack) / 1000.0, p(Decay) / 1000.0, p(Sustain), p(Release) / 1000.0);
    tone_.set(dsp::Biquad::Type::LowPass, sr, p(Tone), 0.707);
    const double g = p(Glide) / 1000.0;
    glideCoef_ = g <= 0 ? 0.0 : std::exp(-1.0 / (g * 0.3 * sr));
}

void Bass808::handleEvent(const NoteEvent& e) noexcept {
    if (e.type == NoteEvent::AllNotesOff) {
        gate_ = false;
        heldCount_ = 0;
        env_.noteOff();
        return;
    }
    if (e.type == NoteEvent::NoteOn && e.velocity > 0) {
        int note = e.note;
        if (p(KeyLock) > 0.5f) {
            Key k{static_cast<int>(p(KeyRoot)), scaleTypeByIndex(static_cast<int>(p(KeyScale)))};
            note = k.nearest(note);
        }
        lastNote_ = note;
        const bool slide = e.slide || (p(Legato) > 0.5f && gate_);
        target_ = note + p(Tune) + e.detune;
        if (!slide || !env_.active()) {
            if (p(PhaseReset) > 0.5f) {
                if (env_.active()) declickPending_ = true; // retrigger while sounding: smooth the jump
                phase_ = p(StartPhase) / 360.0;
            }
            pitch_ = target_;
            env_.noteOn();
            sinceTrigger_ = 0;
            holdLeft_ = p(Hold) / 1000.0;
            vel_ = std::clamp(e.velocity, 0.0f, 1.0f);
        }
        gate_ = true;
        if (heldCount_ < 16) held_[heldCount_++] = e.note;
        return;
    }
    if (e.type == NoteEvent::NoteOff || (e.type == NoteEvent::NoteOn && e.velocity <= 0)) {
        int w = 0;
        for (int i = 0; i < heldCount_; ++i)
            if (held_[i] != e.note) held_[w++] = held_[i];
        heldCount_ = w;
        if (heldCount_ == 0) {
            gate_ = false;
            env_.noteOff();
        }
    }
}

void Bass808::render(const AudioBlock& io, int start, int end) noexcept {
    const double sr = sampleRate_;
    const double dt = 1.0 / sr;
    const double penv = p(PitchEnv), pdec = std::max(1e-3, p(PitchEnvDecay) / 1000.0);
    const float sat = p(Saturation), dist = p(Distortion), sub = p(SubLevel);
    const bool clip = p(SoftClip) > 0.5f;
    const float vol = dbToGain(p(Volume));
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (int i = start; i < end; ++i) {
        if (!env_.active()) {
            sinceTrigger_ += dt;
            lastOut_ = 0.0f;
            continue;
        }
        pitch_ = glideCoef_ > 0 ? target_ + (pitch_ - target_) * glideCoef_ : target_;
        const double bend = penv * std::exp(-sinceTrigger_ / pdec);
        const double f = midiToHz(pitch_ + bend);
        phase_ += f * dt;
        if (phase_ >= 1.0) phase_ -= 1.0;
        const double s1 = std::sin(kTwoPi * phase_);
        double x = sub * s1 + (1.0 - sub) * (0.6 * s1 + 0.3 * std::sin(2 * kTwoPi * phase_) + 0.1 * std::sin(3 * kTwoPi * phase_));
        float a;
        if (env_.stage() == dsp::Adsr::Stage::Decay && holdLeft_ > 0) { // AHDSR hold stage
            holdLeft_ -= dt;
            a = env_.value();
        } else {
            a = env_.next();
        }
        x *= a * vel_;
        if (sat > 0) x = std::tanh(x * (1.0 + 4.0 * sat)) / std::tanh(1.0 + 4.0 * sat);
        if (dist > 0) {
            const double d = 1.0 + 20.0 * dist;
            x = (x >= 0 ? std::tanh(x * d) : std::tanh(x * d * 0.7) / 0.7 * 0.8) / std::tanh(d);
        }
        float y = tone_.process(static_cast<float>(x));
        y = dc_.process(y);
        if (declickPending_) {
            declickOffset_ = lastOut_ - y;
            declickLeft_ = std::max(1, static_cast<int>(0.002 * sr));
            declickPending_ = false;
        }
        if (declickLeft_ > 0) {
            y += declickOffset_ * static_cast<float>(declickLeft_) / static_cast<float>(std::max(1, static_cast<int>(0.002 * sr)));
            --declickLeft_;
        }
        lastOut_ = y;
        y *= vol;
        if (clip) y = std::tanh(y * 1.2f) / std::tanh(1.2f);
        L[i] += y;
        R[i] += y;
        sinceTrigger_ += dt;
    }
}

} // namespace roy
