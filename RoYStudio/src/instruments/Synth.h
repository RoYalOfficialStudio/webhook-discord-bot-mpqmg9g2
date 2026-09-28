#pragma once
// RoY Synth: 16-voice polyphonic subtractive synthesizer.
// 2 band-limited oscillators (saw/square/triangle/sine), ZDF lowpass with
// envelope, amp ADSR, glide, pitch bend, mono/legato mode.
#include "dsp/Filters.h"
#include "instruments/Instrument.h"

#include <array>
#include <cstdint>

namespace roy {

class RoySynth : public Instrument {
public:
    enum P { Osc1Wave, Osc2Wave, Osc2Detune, Osc2Semi, OscMix, Attack, Decay, Sustain, Release, Cutoff, Resonance,
             FilterEnv, Glide, Mono, BendRange, Volume, NumParams };
    RoySynth();
    std::string typeId() const override { return "roy.synth"; }
    std::string displayName() const override { return "RoY Synth"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    double tailSeconds() const override { return 2.0; }
    int activeVoices() const;

protected:
    void beginBlock(int numFrames) noexcept override;
    void render(const AudioBlock& io, int start, int end) noexcept override;
    void handleEvent(const NoteEvent& e) noexcept override;

private:
    struct Voice {
        int note = -1;
        uint64_t age = 0;
        float velocity = 0;
        double phase1 = 0, phase2 = 0;
        double pitch = 60, targetPitch = 60;
        double tri1 = 0, tri2 = 0;
        dsp::Adsr amp, filt;
        dsp::Svf filter;
        bool held = false;
    };
    double oscillator(int wave, double& phase, double inc, double& triState) noexcept;
    Voice* allocate(int note) noexcept;
    void startVoice(Voice& v, const NoteEvent& e, bool legato) noexcept;

    std::array<Voice, 16> voices_;
    uint64_t counter_ = 0;
    double bend_ = 0; // semitones
    double glideCoef_ = 0;
    int heldStack_[128];
    int heldCount_ = 0;
};

} // namespace roy
