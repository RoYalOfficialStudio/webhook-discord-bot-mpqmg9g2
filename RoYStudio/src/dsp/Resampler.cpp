#include "dsp/Resampler.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

namespace {
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double y = x * x / 4.0;
    for (int k = 1; k < 50; ++k) {
        term *= y / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
} // namespace

std::vector<float> resample(const std::vector<float>& in, double srIn, double srOut, int quality) {
    if (in.empty() || srIn <= 0 || srOut <= 0) return {};
    if (std::fabs(srIn - srOut) < 1e-9) return in;
    quality = std::clamp(quality, 8, 64);
    const double ratio = srOut / srIn;
    const double cutoff = std::min(1.0, ratio) * 0.97; // relative to input Nyquist
    const double beta = 8.6;
    const double i0b = besselI0(beta);
    const int half = static_cast<int>(std::ceil(quality / std::min(1.0, cutoff)));

    // Precompute a finely sampled kernel table for speed.
    const int tableRes = 512;
    std::vector<double> table(static_cast<size_t>(half * tableRes + 2));
    for (size_t i = 0; i < table.size(); ++i) {
        const double x = static_cast<double>(i) / tableRes; // in input samples
        double s = x == 0.0 ? 1.0 : std::sin(kPi * x * cutoff) / (kPi * x * cutoff);
        const double r = x / half;
        const double w = r >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
        table[i] = s * w * cutoff;
    }
    auto kernel = [&](double x) {
        x = std::fabs(x) * tableRes;
        const size_t i = static_cast<size_t>(x);
        if (i + 1 >= table.size()) return 0.0;
        const double f = x - static_cast<double>(i);
        return table[i] + (table[i + 1] - table[i]) * f;
    };

    const size_t outLen = static_cast<size_t>(std::llround(static_cast<double>(in.size()) * ratio));
    std::vector<float> out(outLen);
    const int64_t n = static_cast<int64_t>(in.size());
    for (size_t o = 0; o < outLen; ++o) {
        const double pos = static_cast<double>(o) / ratio;
        const int64_t centre = static_cast<int64_t>(std::floor(pos));
        double acc = 0.0;
        for (int64_t k = centre - half + 1; k <= centre + half; ++k) {
            if (k < 0 || k >= n) continue;
            acc += in[static_cast<size_t>(k)] * kernel(pos - static_cast<double>(k));
        }
        out[o] = static_cast<float>(acc);
    }
    return out;
}

} // namespace roy::dsp
