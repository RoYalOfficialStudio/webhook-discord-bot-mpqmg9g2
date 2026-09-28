#include "dsp/Oversampler.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

namespace {
double i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; ++k) {
        t *= (x / 2) * (x / 2) / (k * k);
        s += t;
    }
    return s;
}
} // namespace

void Oversampler4x::prepare(int maxBlock) {
    const int N = kFactor * kTapsPerPhase;
    const double beta = 8.0, cutoff = 0.46 / kFactor; // cycles per high-rate sample (0.46 x base rate)
    double sum = 0;
    for (int i = 0; i < N; ++i) {
        const double m = i - (N - 1) / 2.0;
        const double sinc = m == 0 ? 1.0 : std::sin(kPi * 2 * cutoff * m) / (kPi * 2 * cutoff * m);
        const double r = 2.0 * i / (N - 1) - 1.0;
        const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
        h_[static_cast<size_t>(i)] = static_cast<float>(2 * cutoff * sinc * w);
        sum += h_[static_cast<size_t>(i)];
    }
    for (auto& v : h_) v = static_cast<float>(v / sum); // unity DC gain
    work_.assign(static_cast<size_t>(maxBlock * kFactor), 0.0f);
    reset();
    init_ = true;
}

void Oversampler4x::reset() {
    upHist_.fill(0.0f);
    downHist_.fill(0.0f);
    downPos_ = 0;
}

void Oversampler4x::up(const float* in, float* out, int n) noexcept {
    // zero-stuffing + FIR, computed polyphase: out[k*4+p] = 4 * sum_j h[j*4+p] * x[k-j]
    for (int k = 0; k < n; ++k) {
        for (int j = kTapsPerPhase - 1; j > 0; --j) upHist_[static_cast<size_t>(j)] = upHist_[static_cast<size_t>(j - 1)];
        upHist_[0] = in[k];
        for (int p = 0; p < kFactor; ++p) {
            float acc = 0;
            for (int j = 0; j < kTapsPerPhase; ++j) acc += h_[static_cast<size_t>(j * kFactor + p)] * upHist_[static_cast<size_t>(j)];
            out[k * kFactor + p] = acc * kFactor;
        }
    }
}

void Oversampler4x::down(const float* in, float* out, int n) noexcept {
    const int N = kFactor * kTapsPerPhase;
    for (int k = 0; k < n; ++k) {
        for (int p = 0; p < kFactor; ++p) {
            downHist_[static_cast<size_t>(downPos_)] = in[k * kFactor + p];
            downPos_ = (downPos_ + 1) % N;
        }
        float acc = 0;
        for (int i = 0; i < N; ++i) {
            const int idx = (downPos_ - 1 - i + N * 2) % N;
            acc += h_[static_cast<size_t>(i)] * downHist_[static_cast<size_t>(idx)];
        }
        out[k] = acc;
    }
}

} // namespace roy::dsp
