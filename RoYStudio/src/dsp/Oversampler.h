#pragma once
// 4x polyphase FIR oversampler (Kaiser-windowed sinc, 128 taps at the high rate, passband ~0.46 fs)
// for alias-reduced nonlinear processing. Allocation-free after prepare().
#include <array>
#include <vector>

namespace roy::dsp {

class Oversampler4x {
public:
    static constexpr int kFactor = 4;
    static constexpr int kTapsPerPhase = 32;
    void prepare(int maxBlock);
    void reset();
    // upsample n input samples into out (n*4)
    void up(const float* in, float* out, int n) noexcept;
    // downsample n*4 samples into n
    void down(const float* in, float* out, int n) noexcept;
    // Linear-phase up + down filters: 2 x 63.5 samples at 4x = 31.75 base samples.
    int latencySamples() const { return (kFactor * kTapsPerPhase - 1) * 2 / kFactor / 2 + 1; }
    float* buffer() { return work_.data(); }

private:
    std::array<float, kFactor * kTapsPerPhase> h_{};
    std::array<float, kTapsPerPhase> upHist_{};
    std::array<float, kFactor * kTapsPerPhase> downHist_{};
    int downPos_ = 0;
    std::vector<float> work_;
    bool init_ = false;
};

} // namespace roy::dsp
