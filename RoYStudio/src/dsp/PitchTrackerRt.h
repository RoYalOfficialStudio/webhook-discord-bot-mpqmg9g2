#pragma once
// GMB 13.1 - REALTIME PITCH TRACKER for VocalTune 2.
//
// * anti-alias low-pass (2 x RBJ biquad, 4th order) before decimating to about 16 kHz
// * YIN cumulative-mean-normalised difference, 24 ms window, new estimate every ~4 ms
// * the YIN work is SPREAD over the samples of a hop (bounded work per sample, no burst every hop)
// * confidence = 1 - d'(tau), voiced/unvoiced with hysteresis, level gate, octave-jump guard,
// Realtime safe after prepare(): no allocation, no locks.
#include "dsp/Filters.h"

#include <vector>

namespace roy::dsp {

class PitchTrackerRt {
public:
    void prepare(double sampleRate, double minHz = 80.0, double maxHz = 1000.0);
    void reset();
    // one input sample (mono); true when a new estimate was finished in this call
    bool push(float x) noexcept;

    bool voiced() const { return voiced_; }
    double hz() const { return hz_; }
    double midi() const { return midi_; }
    double confidence() const { return conf_; }
    double hopSeconds() const { return hop_ * decim_ / sr_; }
    // the estimate describes the signal centred about this many input samples ago (window centre +
    // the hop over which the YIN work is spread)
    int analysisDelaySamples() const { return static_cast<int>((W_ / 2 + hop_) * decim_); }
    double minHz() const { return minHz_; }

private:
    void startFrame() noexcept;
    void work(int lags) noexcept;
    void finish() noexcept;

    double sr_ = 48000, fs_ = 16000, minHz_ = 80, maxHz_ = 1000;
    int decim_ = 3, decimCount_ = 0;
    Biquad aa_[2];
    std::vector<float> ring_, frame_, diff_, raw_;
    int ringPos_ = 0, ringFill_ = 0;
    int W_ = 384, maxLag_ = 200, minLag_ = 16, hop_ = 64, sinceHop_ = 0;
    int lagCursor_ = 0, lagsPerSample_ = 4;
    bool frameActive_ = false;
    double running_ = 0, energy_ = 0;
    // result
    bool voiced_ = false;
    int voicedRun_ = 0;
    double hz_ = 0, midi_ = 0, conf_ = 0, lastMidi_ = 0;
};

} // namespace roy::dsp
