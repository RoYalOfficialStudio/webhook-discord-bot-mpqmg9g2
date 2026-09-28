#include "dsp/Loudness.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

// K-weighting coefficients derived for any sample rate (same analog prototype
// as the BS.1770 48 kHz reference coefficients).
void KWeighting::prepare(double fs) {
    {
        const double G = 3.99984385397, fc = 1681.974450955533, Q = 0.7071752369554196;
        const double K = std::tan(kPi * fc / fs);
        const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf_.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf_.b1 = 2.0 * (K * K - Vh) / a0;
        shelf_.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf_.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf_.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double fc = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * fc / fs);
        const double a0 = 1.0 + K / Q + K * K;
        hp_.b0 = 1.0;
        hp_.b1 = -2.0;
        hp_.b2 = 1.0;
        hp_.a1 = 2.0 * (K * K - 1.0) / a0;
        hp_.a2 = (1.0 - K / Q + K * K) / a0;
    }
    reset();
}

float KWeighting::Stage::run(float x) noexcept {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    z1 = sanitize(z1);
    z2 = sanitize(z2);
    return static_cast<float>(y);
}

float KWeighting::process(float x) noexcept { return hp_.run(shelf_.run(x)); }
void KWeighting::reset() noexcept { shelf_.z1 = shelf_.z2 = hp_.z1 = hp_.z2 = 0; }

namespace {
double toLufs(double meanSquare) { return meanSquare <= 1e-20 ? -70.0 : -0.691 + 10.0 * std::log10(meanSquare); }
}

void LoudnessMeter::prepare(double sr, int maxBlock, double maxSeconds) {
    sr_ = sr;
    for (auto& k : kw_) k.prepare(sr);
    for (auto& o : os_) o.prepare(maxBlock);
    subLen_ = static_cast<int>(std::lround(sr * 0.1));
    subRing_.assign(30, 0.0);
    const size_t maxBlocks = static_cast<size_t>(maxSeconds * 10.0) + 64;
    blocks400_.assign(maxBlocks, -70.0f);
    blocks3s_.assign(maxBlocks, -70.0f);
    reset();
}

void LoudnessMeter::reset() {
    for (auto& k : kw_) k.reset();
    for (auto& o : os_) o.reset();
    subPos_ = 0;
    subAcc_ = 0;
    std::fill(subRing_.begin(), subRing_.end(), 0.0);
    subCount_ = 0;
    nBlocks_.store(0);
    nShort_.store(0);
    momentary_.store(-70.0);
    shortTerm_.store(-70.0);
    truePeak_.store(0.0f);
    samplePeak_.store(0.0f);
    sumSq_ = 0;
    samples_ = 0;
    rms_.store(0.0);
    momentaryMax_ = shortMax_ = -70.0;
}

void LoudnessMeter::process(const float* L, const float* R, int n) noexcept {
    float tp = truePeak_.load(std::memory_order_relaxed), sp = samplePeak_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const float l = L[i], r = R ? R[i] : L[i];
        sp = std::max(sp, std::max(std::fabs(l), std::fabs(r)));
        float up[4];
        os_[0].up(&l, up, 1);
        for (float u : up) tp = std::max(tp, std::fabs(u));
        os_[1].up(&r, up, 1);
        for (float u : up) tp = std::max(tp, std::fabs(u));
        sumSq_ += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
        ++samples_;
        const float kl = kw_[0].process(l), kr = kw_[1].process(r);
        subAcc_ += static_cast<double>(kl) * kl + static_cast<double>(kr) * kr; // channel weights G = 1
        if (++subPos_ >= subLen_) {
            subRing_[static_cast<size_t>(subCount_ % 30)] = subAcc_ / subLen_;
            ++subCount_;
            subAcc_ = 0;
            subPos_ = 0;
            if (subCount_ >= 4) {
                double e = 0;
                for (int k = 0; k < 4; ++k) e += subRing_[static_cast<size_t>((subCount_ - 1 - k) % 30)];
                const double m = toLufs(e / 4.0);
                momentary_.store(m, std::memory_order_relaxed);
                momentaryMax_ = std::max(momentaryMax_, m);
                const int b = nBlocks_.load(std::memory_order_relaxed);
                if (b < static_cast<int>(blocks400_.size())) {
                    blocks400_[static_cast<size_t>(b)] = static_cast<float>(m);
                    nBlocks_.store(b + 1, std::memory_order_release);
                }
            }
            if (subCount_ >= 30) {
                double e = 0;
                for (int k = 0; k < 30; ++k) e += subRing_[static_cast<size_t>(k)];
                const double s = toLufs(e / 30.0);
                shortTerm_.store(s, std::memory_order_relaxed);
                shortMax_ = std::max(shortMax_, s);
                const int b = nShort_.load(std::memory_order_relaxed);
                if (b < static_cast<int>(blocks3s_.size())) {
                    blocks3s_[static_cast<size_t>(b)] = static_cast<float>(s);
                    nShort_.store(b + 1, std::memory_order_release);
                }
            }
        }
    }
    truePeak_.store(tp, std::memory_order_relaxed);
    samplePeak_.store(sp, std::memory_order_relaxed);
    rms_.store(samples_ ? std::sqrt(sumSq_ / static_cast<double>(samples_)) : 0.0, std::memory_order_relaxed);
}

double LoudnessMeter::truePeakDb() const { return gainToDb(static_cast<double>(truePeak_.load())); }
double LoudnessMeter::samplePeakDb() const { return gainToDb(static_cast<double>(samplePeak_.load())); }

double LoudnessMeter::integratedLufs() const {
    const int n = nBlocks_.load(std::memory_order_acquire);
    // absolute gate -70 LUFS
    double sum = 0;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
        if (blocks400_[static_cast<size_t>(i)] > -70.0f) {
            sum += std::pow(10.0, (blocks400_[static_cast<size_t>(i)] + 0.691) / 10.0);
            ++cnt;
        }
    if (!cnt) return -70.0;
    const double rel = toLufs(sum / cnt) - 10.0;
    double s2 = 0;
    int c2 = 0;
    for (int i = 0; i < n; ++i) {
        const float v = blocks400_[static_cast<size_t>(i)];
        if (v > -70.0f && v > rel) {
            s2 += std::pow(10.0, (v + 0.691) / 10.0);
            ++c2;
        }
    }
    return c2 ? toLufs(s2 / c2) : -70.0;
}

double LoudnessMeter::loudnessRange() const {
    const int n = nShort_.load(std::memory_order_acquire);
    std::vector<float> v;
    double sum = 0;
    for (int i = 0; i < n; ++i)
        if (blocks3s_[static_cast<size_t>(i)] > -70.0f) {
            v.push_back(blocks3s_[static_cast<size_t>(i)]);
            sum += std::pow(10.0, (blocks3s_[static_cast<size_t>(i)] + 0.691) / 10.0);
        }
    if (v.size() < 2) return 0.0;
    const double rel = toLufs(sum / static_cast<double>(v.size())) - 20.0;
    std::erase_if(v, [&](float x) { return x < rel; });
    if (v.size() < 2) return 0.0;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[static_cast<size_t>(std::lround(p * static_cast<double>(v.size() - 1)))]; };
    return pct(0.95) - pct(0.10);
}

LoudnessStats LoudnessMeter::stats() const {
    LoudnessStats s;
    s.integratedLufs = integratedLufs();
    s.momentaryMaxLufs = momentaryMax_;
    s.shortTermMaxLufs = shortMax_;
    s.loudnessRangeLu = loudnessRange();
    s.samplePeakDb = samplePeakDb();
    s.truePeakDb = truePeakDb();
    s.rmsDb = gainToDb(rms_.load());
    s.dynamicRangeDb = s.truePeakDb - s.integratedLufs;
    s.crestDb = s.samplePeakDb - s.rmsDb;
    return s;
}

LoudnessStats measureLoudness(const std::vector<std::vector<float>>& ch, double sr) {
    LoudnessMeter m;
    const size_t n = ch.empty() ? 0 : ch[0].size();
    m.prepare(sr, 4096, static_cast<double>(n) / sr + 10.0);
    const float* L = ch.empty() ? nullptr : ch[0].data();
    const float* R = ch.size() > 1 ? ch[1].data() : L;
    for (size_t off = 0; off < n; off += 4096) {
        const int k = static_cast<int>(std::min<size_t>(4096, n - off));
        m.process(L + off, R + off, k);
    }
    return m.stats();
}

double normalizationGainDb(const LoudnessStats& s, double target, double ceiling) {
    double g = target - s.integratedLufs;
    if (s.truePeakDb + g > ceiling) g = ceiling - s.truePeakDb; // never push peaks over the ceiling
    return g;
}

} // namespace roy::dsp
