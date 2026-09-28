#pragma once
// RoY EQ, space, modulation, saturation and stereo processors.
#include "audio/Processor.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"

#include <array>
#include <atomic>
#include <vector>

namespace roy {

// 4-band parametric EQ (low shelf, 2 peaks, high shelf) + low/high cut.
class RoyEq : public Processor {
public:
    enum P { LowCut, LowCutSlope, HighCut, B1Freq, B1Gain, B1Q, B2Freq, B2Gain, B2Q, B3Freq, B3Gain, B3Q, B4Freq, B4Gain, B4Q, Output, NumParams };
    RoyEq();
    std::string typeId() const override { return "roy.eq"; }
    std::string displayName() const override { return "RoY EQ"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    // Combined magnitude response in dB at `hz` (for tests / curve display).
    double responseDb(double hz) const;

private:
    void update() noexcept;
    std::array<float, NumParams> cache_{};
    dsp::Biquad lc_[2][2], hc_[2], bands_[2][4];
    bool lcOn_ = false, hcOn_ = false;
    int lcStages_ = 1;
};

// Feedback-delay-network reverb (8 lines, Householder mixing, HF damping).
class RoyReverb : public Processor {
public:
    enum P { Decay, PreDelay, Size, Damping, Width, Mix, LowCut, NumParams };
    RoyReverb();
    std::string typeId() const override { return "roy.reverb"; }
    std::string displayName() const override { return "RoY Reverb"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    double tailSeconds() const override { return p(Decay) * 1.5 + 0.2; }

private:
    static constexpr int kLines = 8;
    std::array<std::vector<float>, kLines> lines_;
    std::array<int, kLines> len_{}, pos_{};
    std::array<float, kLines> lp_{};
    std::vector<float> pre_;
    int prePos_ = 0;
    dsp::Biquad inHp_;
    float lastSize_ = -1;
};

// Stereo delay: time in ms or tempo-synced, feedback with filters, ping-pong.
class RoyDelay : public Processor {
public:
    enum P { TimeMs, SyncBeats, Feedback, Mix, PingPong, LowCut, HighCut, NumParams };
    RoyDelay();
    std::string typeId() const override { return "roy.delay"; }
    std::string displayName() const override { return "RoY Delay"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    void setHostTempo(double bpm) override { bpm_.store(bpm); }
    double tailSeconds() const override { return 5.0; }
    double currentDelaySeconds() const;

private:
    std::vector<float> bufL_, bufR_;
    int pos_ = 0;
    dsp::Biquad lc_[2], hc_[2];
    float lastLc_ = -1, lastHc_ = -1;
    dsp::Smoother time_;
    std::atomic<double> bpm_{120.0};
};

// Chorus / flanger share a modulated delay line.
class RoyModDelay : public Processor {
public:
    enum P { Rate, Depth, Delay, Feedback, Mix, Spread, NumParams };
    RoyModDelay(bool flanger);
    std::string typeId() const override { return flanger_ ? "roy.flanger" : "roy.chorus"; }
    std::string displayName() const override { return flanger_ ? "RoY Flanger" : "RoY Chorus"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;

private:
    bool flanger_;
    std::vector<float> buf_[2];
    int pos_ = 0;
    double phase_ = 0;
    float fb_[2] = {0, 0};
};

class RoyPhaser : public Processor {
public:
    enum P { Rate, Depth, Stages, Feedback, Mix, Centre, NumParams };
    RoyPhaser();
    std::string typeId() const override { return "roy.phaser"; }
    std::string displayName() const override { return "RoY Phaser"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;

private:
    std::array<std::array<float, 12>, 2> z_{};
    double phase_ = 0;
    float fb_[2] = {0, 0};
};

// Stereo tool: width, mono bass below a frequency, balance, mid/side gains.
class RoyStereo : public Processor {
public:
    enum P { Width, MonoBass, Balance, MidGain, SideGain, NumParams };
    RoyStereo();
    std::string typeId() const override { return "roy.stereo"; }
    std::string displayName() const override { return "RoY Stereo"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;

private:
    dsp::Biquad sideHp_, sideHp2_;
    float lastMono_ = -1;
};

// Saturation (tape/tube style) and Distortion, 4x oversampled.
class RoySaturation : public Processor {
public:
    enum P { Drive, Mode, Tone, Mix, Output, NumParams };
    explicit RoySaturation(bool distortion);
    std::string typeId() const override { return distortion_ ? "roy.distortion" : "roy.saturation"; }
    std::string displayName() const override { return distortion_ ? "RoY Distortion" : "RoY Saturation"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept override;
    int latencySamples() const override { return os_[0].latencySamples(); }

private:
    bool distortion_;
    dsp::Oversampler4x os_[2];
    std::vector<float> hi_, wet_;
    std::vector<float> dryLine_[2];
    int dryPos_[2] = {0, 0};
    dsp::Biquad tone_[2];
    dsp::DcBlocker dc_[2];
    float lastTone_ = -1;
};

} // namespace roy
