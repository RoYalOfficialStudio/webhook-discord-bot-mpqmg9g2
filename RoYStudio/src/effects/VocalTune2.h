#pragma once
// GMB 13.3 / 13.4 - "RoY VocalTune 2" (roy.vocaltune2): realtime auto-tune with formant preservation.
//
// Separate effect next to roy.vocaltune (which stays unchanged for old projects and presets).
//   tracker  : dsp::PitchTrackerRt (confidence, hysteresis, spread YIN work)
//   shifter  : dsp::PsolaShifter (TD-PSOLA, fixed latency, formant preserving, formant shift)
//   target   : same OFF-KEY FILTER / ALLOW CHROMATIC / threshold semantics as roy.vocaltune and
//              Pitch Guardian; note hysteresis (no flip-flop between two notes)
//   vibrato  : 1 = only the slow pitch centre (150 ms) is corrected, the vibrato stays
//   humanize : held notes (> 250 ms) are corrected more gently and slower
//   confidence: below this tracker confidence the correction fades out (no jumps on noise/breaths)
//   mode     : Custom (parameters) / Natural / Modern / Hard (0 ms, flat - trap, drill, melodic rap)
//   mix      : dry/wet, the dry path is delayed like the wet path (no comb filter), 20 ms smoothing
// latencySamples() reports the fixed PSOLA latency, so the engine's delay compensation (PDC) lines
// the channel up with the others. Live monitoring hears this latency (1432 samples = 29.8 ms at 48 kHz).
#include "audio/Processor.h"
#include "dsp/PitchTrackerRt.h"
#include "dsp/PsolaShifter.h"

#include <atomic>

namespace roy {

class RoyVocalTune2 : public Processor {
public:
    enum P { KeyRoot, KeyScale, Speed, Strength, OffKeyFilter, AllowChromatic, Threshold,
             Mix, Formant, FormantShift, Humanize, Vibrato, Confidence, Mode, NumParams };
    enum Modes { Custom = 0, Natural = 1, Modern = 2, Hard = 3 };
    RoyVocalTune2();
    std::string typeId() const override { return "roy.vocaltune2"; }
    std::string displayName() const override { return "RoY VocalTune 2"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    int latencySamples() const override { return psola_.latencySamples(); }

    double detectedMidi() const { return detected_.load(std::memory_order_relaxed); }
    double appliedShift() const { return shiftOut_.load(std::memory_order_relaxed); }
    double trackerConfidence() const { return confOut_.load(std::memory_order_relaxed); }

private:
    void updateTarget() noexcept;
    dsp::PitchTrackerRt tracker_;
    dsp::PsolaShifter psola_;
    double shift_ = 0, targetShift_ = 0, center_ = 0, note_ = -1000, heldSec_ = 0, effSpeedMs_ = 40;
    double ratio_ = 1, formant_ = 1;
    float mixNow_ = 1;
    int sinceRatio_ = 0, lastSide_ = 0;
    double sideRunSec_ = 0;
    bool wasVoiced_ = false;
    std::atomic<double> detected_{0.0}, shiftOut_{0.0}, confOut_{0.0};
};

} // namespace roy
