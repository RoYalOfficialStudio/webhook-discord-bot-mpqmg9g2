#pragma once
// 808 LAB instrument: monophonic sub bass with glide/slide, pitch envelope,
// AHDSR, saturation, distortion, soft clip, sub control and a realtime
// 808 KEY LOCK (incoming notes are snapped to the selected scale).
// PHASE: with Phase Reset on, every new (non-slide) note starts at Start Phase, so the
// 808 lines up with the kick the same way on every hit; a 2 ms de-click ramp hides the
// jump when a still-sounding note is retriggered.
#include "dsp/Filters.h"
#include "instruments/Instrument.h"
#include "midi/Scale.h"

namespace roy {

class Bass808 : public Instrument {
public:
    enum P { Tune, PitchEnv, PitchEnvDecay, Glide, Attack, Hold, Decay, Sustain, Release, Saturation, Distortion, SoftClip,
             SubLevel, Tone, Legato, KeyLock, KeyRoot, KeyScale, Volume, StartPhase, PhaseReset, NumParams };
    Bass808();
    std::string typeId() const override { return "roy.808"; }
    std::string displayName() const override { return "RoY 808"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    double tailSeconds() const override { return 4.0; }
    // For tests/UI: current oscillator pitch (MIDI) and whether a note sounds.
    double currentPitch() const { return pitch_; }
    bool sounding() const { return env_.active(); }
    int lastPlayedNote() const { return lastNote_; }

protected:
    void beginBlock(int numFrames) noexcept override;
    void render(const AudioBlock& io, int start, int end) noexcept override;
    void handleEvent(const NoteEvent& e) noexcept override;

private:
    dsp::Adsr env_;
    dsp::Biquad tone_;
    dsp::DcBlocker dc_;
    double phase_ = 0;
    double pitch_ = 36, target_ = 36;
    double glideCoef_ = 0;
    double sinceTrigger_ = 1e9;
    double holdLeft_ = 0;
    bool gate_ = false;
    int held_[16];
    int heldCount_ = 0;
    int lastNote_ = -1;
    float vel_ = 1;
    float lastOut_ = 0;       // last output sample (before volume) for the de-click ramp
    bool declickPending_ = false;
    float declickOffset_ = 0;
    int declickLeft_ = 0;
};

} // namespace roy
