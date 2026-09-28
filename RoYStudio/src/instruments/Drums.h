#pragma once
// RoY Drums: drum machine for the Beat Lab.
// Synthesised voices per note (GM-style mapping) and optional sample pads.
//   35 808 | 36 kick | 37 rim | 38 snare | 39 clap | 42 closed hat
//   46 open hat (choked by 42) | 47 perc/tom | 49 FX/crash
#include "audio/RenderGraph.h"
#include "dsp/Filters.h"
#include "instruments/Instrument.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace roy {

class RoyDrums : public Instrument {
public:
    enum P { Volume, KickTune, KickDecay, KickClick, SnareTone, SnareDecay, ClapDecay, HatDecay, OpenHatDecay, PercTune, Width, NumParams };
    RoyDrums();
    std::string typeId() const override { return "roy.drums"; }
    std::string displayName() const override { return "RoY Drums"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    double tailSeconds() const override { return 2.0; }
    // Message thread: assigns a sample to a note (nullptr = synthesised voice).
    void setSample(int note, std::shared_ptr<const AudioData> sample);
    int activeVoices() const;

protected:
    void render(const AudioBlock& io, int start, int end) noexcept override;
    void handleEvent(const NoteEvent& e) noexcept override;

private:
    enum class Kind { None, Kick, Snare, Clap, ClosedHat, OpenHat, Perc, Rim, Sub808, Fx, Sample };
    struct Voice {
        Kind kind = Kind::None;
        int note = 0;
        uint64_t age = 0;
        double t = 0;          // seconds since trigger
        float vel = 1, panL = 1, panR = 1;
        double pitchRatio = 1;
        double phase = 0, phase2 = 0;
        dsp::Biquad f1, f2;
        const AudioData* sample = nullptr;
        double samplePos = 0;
        uint32_t noise = 1;
    };
    float noise(Voice& v) noexcept;
    float renderVoice(Voice& v, double dt) noexcept; // mono sample, returns 0 and sets None when finished

    std::array<Voice, 32> voices_;
    uint64_t counter_ = 0;
    std::array<std::atomic<const AudioData*>, 128> samples_{};
    std::vector<std::shared_ptr<const AudioData>> keepAlive_; // message thread only
    std::mutex keepMutex_;
};

} // namespace roy
