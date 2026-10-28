#pragma once
// GMB 13.2 - REALTIME TD-PSOLA PITCH SHIFTER with FIXED latency (VocalTune 2).
//
// Pitch-synchronous overlap-add: analysis marks are placed exactly one (fractional) period apart on
// the input; output grains (Hann, two periods long, read with interpolation) are taken from the
// nearest analysis mark and placed at synthesis marks spaced period / ratio (fractional, so the
// output pitch is exact - no rounding of the period). The grain shape - and so the spectral
// envelope (formants) - is kept: FORMANT PRESERVATION by construction. A formant factor resamples the
// grain (1 = keep the voice colour, > 1 brighter / smaller, < 1 darker / bigger).
// Unvoiced input or ratio 1 with formant 1: synthesis marks = analysis marks -> transparent (exact
// reconstruction, only delayed).
// PARAMETER ALIGNMENT: a pitch tracker describes the signal of `analysisDelay` samples ago. The
// period / ratio / formant passed with sample n are therefore stored for time n - analysisDelay, and
// marks and grains are only placed once their parameters are known - the correction lands on the
// audio it was measured on (vibrato, note changes).
// LATENCY is fixed: max(analysisDelay, longest period) + longest period + 64 samples. At 48 kHz,
// minHz = 80 Hz, no analysis delay: 1264 samples (26.3 ms); with the VocalTune 2 tracker (768): 1432. The dry signal is delayed by exactly the same amount, so a dry/wet mix
// has no comb filter. Ratio and formant factor are limited to 0.7 .. 1.42 (-6 .. +6 semitones).
// Up to 2 channels share the marks. Realtime safe after prepare().
#include <cstdint>
#include <vector>

namespace roy::dsp {

class PsolaShifter {
public:
    static constexpr double kMinRatio = 0.7, kMaxRatio = 1.42;
    void prepare(double sampleRate, int channels, double minHz = 80.0, int analysisDelay = 0);
    void reset();
    int latencySamples() const { return L_; }
    // one frame: in[ch] -> wet[ch] (shifted) and dry[ch] (the input delayed by latencySamples()).
    // period = input period in samples (0 = unvoiced/unknown), ratio = output/input pitch.
    void process(const float* in, float* wet, float* dry, double period, double ratio, double formantFactor) noexcept;

private:
    struct Mark {
        double pos = 0;
        double period = 0;
    };
    float& at(std::vector<float>& v, int64_t i) noexcept { return v[static_cast<size_t>(i & mask_)]; }
    float read(const std::vector<float>& v, double pos) const noexcept;
    void addGrain(double centre, const Mark& src, double formant) noexcept;

    double sr_ = 48000;
    int ch_ = 2, L_ = 1264, Pmax_ = 600, Pdef_ = 256;
    int64_t mask_ = 0, n_ = 0;
    double nextMark_ = 0, nextSyn_ = -1;
    std::vector<float> in_[2], oa_[2], ws_, parPeriod_, parRatio_, parFormant_;
    int D_ = 0;
    Mark marks_[128];
    int markHead_ = 0, markCount_ = 0, markCur_ = 0; // ring of the most recent marks
};

} // namespace roy::dsp
