#pragma once
// Loudness metering per ITU-R BS.1770-4 / EBU R128:
// K-weighting, momentary (400 ms), short-term (3 s), integrated (gated),
// loudness range (LRA), sample peak, true peak (4x oversampled), RMS.
// process() is realtime-safe (all storage allocated in prepare()).
#include "dsp/Oversampler.h"

#include <atomic>
#include <vector>

namespace roy::dsp {

class KWeighting {
public:
    void prepare(double sr);
    float process(float x) noexcept;
    void reset() noexcept;

private:
    struct Stage {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float run(float x) noexcept;
    } shelf_, hp_;
};

struct LoudnessStats {
    double integratedLufs = -70.0;
    double momentaryMaxLufs = -70.0;
    double shortTermMaxLufs = -70.0;
    double loudnessRangeLu = 0.0;
    double samplePeakDb = -120.0;
    double truePeakDb = -120.0;
    double rmsDb = -120.0;
    double dynamicRangeDb = 0.0; // peak-to-loudness ratio (true peak - integrated)
    double crestDb = 0.0;        // sample peak - RMS
};

class LoudnessMeter {
public:
    void prepare(double sr, int maxBlock, double maxSeconds = 3.0 * 3600.0);
    void reset();
    void process(const float* L, const float* R, int n) noexcept;

    double momentaryLufs() const { return momentary_.load(std::memory_order_relaxed); }
    double shortTermLufs() const { return shortTerm_.load(std::memory_order_relaxed); }
    double truePeakDb() const;
    double samplePeakDb() const;
    // Message thread: gated integrated loudness and LRA over everything since reset().
    double integratedLufs() const;
    double loudnessRange() const;
    LoudnessStats stats() const;

private:
    double sr_ = 48000;
    KWeighting kw_[2];
    Oversampler4x os_[2];
    int subLen_ = 4800; // 100 ms
    int subPos_ = 0;
    double subAcc_ = 0;
    std::vector<double> subRing_; // last 30 x 100 ms energies (mean square per sub-block)
    int subCount_ = 0;
    std::vector<float> blocks400_; // loudness of each 400 ms block (75 % overlap)
    std::vector<float> blocks3s_;  // short-term values every 100 ms
    std::atomic<int> nBlocks_{0}, nShort_{0};
    std::atomic<double> momentary_{-70.0}, shortTerm_{-70.0};
    std::atomic<float> truePeak_{0.0f}, samplePeak_{0.0f};
    double sumSq_ = 0;
    uint64_t samples_ = 0;
    std::atomic<double> rms_{0.0};
    double momentaryMax_ = -70.0, shortMax_ = -70.0;
};

// Offline convenience.
LoudnessStats measureLoudness(const std::vector<std::vector<float>>& channels, double sampleRate);
// Gain (dB) that brings `stats.integratedLufs` to target while keeping true peak <= ceiling.
double normalizationGainDb(const LoudnessStats& stats, double targetLufs, double truePeakCeilingDb);

} // namespace roy::dsp
