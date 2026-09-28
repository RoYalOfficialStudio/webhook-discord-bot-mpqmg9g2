#pragma once
// RoY dynamics processors.
#include "audio/Processor.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"

#include <array>
#include <atomic>
#include <vector>

namespace roy {

// Feed-forward compressor, soft knee, optional external sidechain, parallel mix.
class RoyCompressor : public Processor {
public:
    enum P { Threshold, Ratio, Attack, Release, Knee, Makeup, Mix, UseSidechain, ScHighPass, NumParams };
    RoyCompressor();
    std::string typeId() const override { return "roy.compressor"; }
    std::string displayName() const override { return "RoY Compressor"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept override;
    float gainReductionDb() const { return gr_.load(std::memory_order_relaxed); }
    // Static curve (for tests / UI): output level for an input level (dB).
    double curveDb(double inDb) const;

private:
    double env_ = 0;
    dsp::Biquad scHp_[2];
    std::atomic<float> gr_{0.0f};
};

// Lookahead brickwall limiter with optional true-peak (4x) detection.
class RoyLimiter : public Processor {
public:
    enum P { Ceiling, Release, Lookahead, TruePeak, InputGain, NumParams };
    RoyLimiter();
    std::string typeId() const override { return "roy.limiter"; }
    std::string displayName() const override { return "RoY Limiter"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    int latencySamples() const override { return lookahead_; }
    float gainReductionDb() const { return gr_.load(std::memory_order_relaxed); }

private:
    int lookahead_ = 72;
    std::vector<float> delayL_, delayR_, peakBuf_;
    int pos_ = 0;
    double gain_ = 1.0;
    dsp::Oversampler4x osL_, osR_;
    std::atomic<float> gr_{0.0f};
};

// Noise gate / expander with hold, range and optional sidechain.
class RoyGate : public Processor {
public:
    enum P { Threshold, Range, Attack, Hold, Release, UseSidechain, NumParams };
    RoyGate();
    std::string typeId() const override { return "roy.gate"; }
    std::string displayName() const override { return "RoY Gate"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock* sc, const NoteEvent*, int) noexcept override;
    bool isOpen() const { return open_; }

private:
    double env_ = 0, gain_ = 0, holdLeft_ = 0;
    bool open_ = false;
};

// De-esser: a dynamic high shelf above `frequency` (only the sibilant range is
// reduced, and only while sibilance exceeds the threshold). Flat when idle.
class RoyDeEsser : public Processor {
public:
    enum P { Frequency, Threshold, Range, Listen, NumParams };
    RoyDeEsser();
    std::string typeId() const override { return "roy.deesser"; }
    std::string displayName() const override { return "RoY De-Esser"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    float gainReductionDb() const { return gr_.load(std::memory_order_relaxed); }

private:
    dsp::Biquad shelf_[2], det_, listenHp_[2];
    dsp::EnvelopeFollower env_;
    float lastFreq_ = -1;
    double gainDb_ = 0;
    int counter_ = 0;
    std::atomic<float> gr_{0.0f};
};

// Transient shaper: boosts/cuts attack and sustain independently of level.
class RoyTransient : public Processor {
public:
    enum P { AttackAmount, SustainAmount, Output, NumParams };
    RoyTransient();
    std::string typeId() const override { return "roy.transient"; }
    std::string displayName() const override { return "RoY Transient"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;

private:
    dsp::EnvelopeFollower fast_, slow_;
};

// Oversampled clipper with adjustable softness.
class RoyClipper : public Processor {
public:
    enum P { Ceiling, Softness, InputGain, NumParams };
    RoyClipper();
    std::string typeId() const override { return "roy.clipper"; }
    std::string displayName() const override { return "RoY Clipper"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    int latencySamples() const override { return os_[0].latencySamples(); }

private:
    dsp::Oversampler4x os_[2];
    std::vector<float> hi_;
};

// Shared static curve for soft clipping, ceiling-normalised (|y| <= ceiling).
float softClip(float x, float ceiling, float softness) noexcept;

} // namespace roy
