#pragma once
// Radix-2 FFT and spectral helpers (analysis, STFT processors).
#include <complex>
#include <vector>

namespace roy::dsp {

using cpx = std::complex<float>;

class FFT {
public:
    explicit FFT(int size = 1024);
    int size() const { return n_; }
    // In-place complex FFT (inverse is unscaled; divide by N yourself).
    void transform(cpx* data, bool inverse) const;
    // Real input of length N -> N/2+1 complex bins.
    void forwardReal(const float* in, cpx* out) const;
    // N/2+1 bins -> N real samples (scaled by 1/N).
    void inverseReal(const cpx* in, float* out) const;

private:
    int n_;
    std::vector<cpx> twiddles_;
    std::vector<int> bitrev_;
    mutable std::vector<cpx> scratch_;
};

std::vector<float> hannWindow(int n, bool periodic = true);
// Magnitude spectrum (N/2+1 bins) of a Hann-windowed frame.
std::vector<float> magnitudeSpectrum(const float* frame, int n);
inline double binToHz(int bin, int fftSize, double sr) { return static_cast<double>(bin) * sr / fftSize; }

} // namespace roy::dsp
