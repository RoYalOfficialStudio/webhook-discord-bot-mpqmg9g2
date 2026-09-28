#pragma once
// RoY NoiseCleaner, Analyzer (meter), Pitch, VocalTune and Dynamic Space.
#include "audio/Processor.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"
#include "dsp/Loudness.h"
#include "midi/Scale.h"

#include <array>
#include <atomic>
#include <vector>

namespace roy {

// STFT spectral noise reduction with a learned/adaptive noise profile.
class RoyNoiseCleaner : public Processor {
public:
    enum P { Reduction, Sensitivity, Learn, Smoothing, NumParams };
    RoyNoiseCleaner();
    std::string typeId() const override { return "roy.noisecleaner"; }
    std::string displayName() const override { return "RoY NoiseCleaner"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    int latencySamples() const override { return kN; }

private:
    static constexpr int kN = 1024, kHop = 256;
    dsp::FFT fft_{kN};
    std::vector<float> win_;
    std::array<std::vector<float>, 2> inFifo_, outAcc_, outFifo_;
    int fifoPos_ = 0;
    std::vector<float> noise_, gainSm_, frame_, smoothP_, curMin_, rawGain_;
    std::vector<std::vector<float>> subMins_; // minimum statistics: 16 sub-windows of 32 frames
    int subIdx_ = 0, frameInSub_ = 0;
    std::vector<dsp::cpx> spec_;
    dsp::cpx specs_[2][kN / 2 + 1];
    int frames_ = 0;
};

// Pass-through analyser: log-spaced spectrum + BS.1770 loudness + correlation.
class RoyAnalyzer : public Processor {
public:
    static constexpr int kBands = 64;
    RoyAnalyzer();
    std::string typeId() const override { return "roy.analyzer"; }
    std::string displayName() const override { return "RoY Analyzer"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    float bandDb(int band) const { return bands_[static_cast<size_t>(band)].load(std::memory_order_relaxed); }
    double bandCentreHz(int band) const;
    float correlation() const { return corr_.load(std::memory_order_relaxed); }
    const dsp::LoudnessMeter& loudness() const { return meter_; }

private:
    static constexpr int kN = 4096;
    dsp::FFT fft_{kN};
    std::vector<float> win_, ring_, frame_;
    std::vector<dsp::cpx> spec_;
    int ringPos_ = 0, sinceFft_ = 0;
    std::array<std::atomic<float>, kBands> bands_;
    std::atomic<float> corr_{1.0f};
    double sLR_ = 0, sLL_ = 0, sRR_ = 0;
    dsp::LoudnessMeter meter_;
};

// Realtime delay-line pitch shifter (two crossfaded taps). Low latency, some
// "chorus" artefacts on large shifts; offline PSOLA (Pitch Guardian) is cleaner.
class PitchShifterRt {
public:
    void prepare(double sr, double windowMs = 40.0);
    void reset();
    float process(float x, double ratio) noexcept;

private:
    std::vector<float> buf_;
    int pos_ = 0;
    double phase_ = 0;
    double sr_ = 48000, window_ = 1920;
};

class RoyPitch : public Processor {
public:
    enum P { Semitones, Cents, Mix, Window, NumParams };
    RoyPitch();
    std::string typeId() const override { return "roy.pitch"; }
    std::string displayName() const override { return "RoY Pitch"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;

private:
    PitchShifterRt sh_[2];
};

// Realtime auto-tune ("VocalTune"): realtime YIN + scale target + speed, same
// OFF-KEY FILTER / ALLOW CHROMATIC semantics as Pitch Guardian.
class RoyVocalTune : public Processor {
public:
    enum P { KeyRoot, KeyScale, Speed, Strength, OffKeyFilter, AllowChromatic, Threshold, NumParams };
    RoyVocalTune();
    std::string typeId() const override { return "roy.vocaltune"; }
    std::string displayName() const override { return "RoY VocalTune"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    double detectedMidi() const { return detected_.load(std::memory_order_relaxed); }
    double appliedShift() const { return shiftOut_.load(std::memory_order_relaxed); }

private:
    void analyse() noexcept;
    PitchShifterRt sh_[2];
    std::vector<float> ring_, frame_, diff_;
    int ringPos_ = 0, sinceAnalysis_ = 0, decim_ = 1;
    float decimAcc_ = 0;
    int decimCount_ = 0;
    double targetShift_ = 0, shift_ = 0;
    std::atomic<double> detected_{0.0}, shiftOut_{0.0};
};

// DYNAMIC SPACE: cuts a frequency band of this channel only while the key
// (sidechain, e.g. the vocal) is active in that band.
class RoyDynamicSpace : public Processor {
public:
    enum P { Frequency, Bandwidth, MaxReduction, Attack, Release, Sensitivity, NumParams };
    RoyDynamicSpace();
    std::string typeId() const override { return "roy.dynamicspace"; }
    std::string displayName() const override { return "RoY Dynamic Space"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept override;
    float currentReductionDb() const { return red_.load(std::memory_order_relaxed); }

private:
    dsp::Biquad detect_[2], eq_[2];
    double env_ = 0, gainDb_ = 0;
    std::atomic<float> red_{0.0f};
    float lastFreq_ = -1, lastBw_ = -1;
    int counter_ = 0;
};

} // namespace roy
