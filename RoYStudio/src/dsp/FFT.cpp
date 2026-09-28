#include "dsp/FFT.h"
#include "core/Math.h"

#include <cmath>

namespace roy::dsp {

FFT::FFT(int size) {
    n_ = 1;
    while (n_ < size) n_ <<= 1;
    twiddles_.resize(static_cast<size_t>(n_ / 2));
    for (int i = 0; i < n_ / 2; ++i)
        twiddles_[static_cast<size_t>(i)] = std::polar(1.0f, static_cast<float>(-kTwoPi * i / n_));
    bitrev_.resize(static_cast<size_t>(n_));
    int bits = 0;
    while ((1 << bits) < n_) ++bits;
    for (int i = 0; i < n_; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev_[static_cast<size_t>(i)] = r;
    }
    scratch_.resize(static_cast<size_t>(n_));
}

void FFT::transform(cpx* d, bool inverse) const {
    for (int i = 0; i < n_; ++i) {
        const int j = bitrev_[static_cast<size_t>(i)];
        if (j > i) std::swap(d[i], d[j]);
    }
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len / 2;
        const int step = n_ / len;
        for (int i = 0; i < n_; i += len) {
            for (int k = 0; k < half; ++k) {
                cpx w = twiddles_[static_cast<size_t>(k * step)];
                if (inverse) w = std::conj(w);
                const cpx a = d[i + k];
                const cpx b = d[i + k + half] * w;
                d[i + k] = a + b;
                d[i + k + half] = a - b;
            }
        }
    }
}

void FFT::forwardReal(const float* in, cpx* out) const {
    for (int i = 0; i < n_; ++i) scratch_[static_cast<size_t>(i)] = cpx(in[i], 0.0f);
    transform(scratch_.data(), false);
    for (int i = 0; i <= n_ / 2; ++i) out[i] = scratch_[static_cast<size_t>(i)];
}

void FFT::inverseReal(const cpx* in, float* out) const {
    for (int i = 0; i <= n_ / 2; ++i) scratch_[static_cast<size_t>(i)] = in[i];
    for (int i = n_ / 2 + 1; i < n_; ++i) scratch_[static_cast<size_t>(i)] = std::conj(in[n_ - i]);
    transform(scratch_.data(), true);
    const float s = 1.0f / static_cast<float>(n_);
    for (int i = 0; i < n_; ++i) out[i] = scratch_[static_cast<size_t>(i)].real() * s;
}

std::vector<float> hannWindow(int n, bool periodic) {
    std::vector<float> w(static_cast<size_t>(n));
    const double d = periodic ? n : std::max(1, n - 1);
    for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / d));
    return w;
}

std::vector<float> magnitudeSpectrum(const float* frame, int n) {
    FFT fft(n);
    const int N = fft.size();
    auto w = hannWindow(N);
    std::vector<float> buf(static_cast<size_t>(N), 0.0f);
    for (int i = 0; i < std::min(n, N); ++i) buf[static_cast<size_t>(i)] = frame[i] * w[static_cast<size_t>(i)];
    std::vector<cpx> spec(static_cast<size_t>(N / 2 + 1));
    fft.forwardReal(buf.data(), spec.data());
    std::vector<float> mag(spec.size());
    for (size_t i = 0; i < spec.size(); ++i) mag[i] = std::abs(spec[i]);
    return mag;
}

} // namespace roy::dsp
