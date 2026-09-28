#include "vocal/PitchDetector.h"
#include "core/Math.h"
#include "dsp/FFT.h"

#include <algorithm>
#include <cmath>

namespace roy::vocal {

size_t PitchTrack::indexAt(double seconds) const {
    if (frames.empty()) return 0;
    const double idx = seconds / hopSeconds();
    return static_cast<size_t>(std::clamp(std::llround(idx), 0LL, static_cast<long long>(frames.size() - 1)));
}

double PitchTrack::midiAt(double seconds) const {
    if (frames.empty()) return 0;
    const double idx = seconds / hopSeconds();
    if (idx <= 0) return frames.front().voiced ? frames.front().midi : 0;
    const size_t i = static_cast<size_t>(idx);
    if (i + 1 >= frames.size()) return frames.back().voiced ? frames.back().midi : 0;
    const auto& a = frames[i];
    const auto& b = frames[i + 1];
    if (!a.voiced || !b.voiced) return a.voiced ? a.midi : (b.voiced ? b.midi : 0);
    const double f = idx - static_cast<double>(i);
    return a.midi + (b.midi - a.midi) * f;
}

PitchTrack detectPitch(const float* x, int64_t n, double sr, const PitchDetectorSettings& s) {
    PitchTrack track;
    track.sampleRate = sr;
    track.hop = s.hop;
    const int W = std::max(64, static_cast<int>(s.windowSeconds * sr));
    const int maxLag = std::min(static_cast<int>(sr / s.minHz) + 2, W * 2);
    const int minLag = std::max(2, static_cast<int>(sr / s.maxHz));
    int M = 1;
    while (M < W + maxLag + W) M <<= 1;
    dsp::FFT fft(M);
    std::vector<float> a(static_cast<size_t>(M)), b(static_cast<size_t>(M));
    std::vector<dsp::cpx> A(static_cast<size_t>(M / 2 + 1)), B(static_cast<size_t>(M / 2 + 1));
    std::vector<float> corr(static_cast<size_t>(M));
    std::vector<double> d(static_cast<size_t>(maxLag + 1)), cmnd(static_cast<size_t>(maxLag + 1));
    std::vector<double> sq(static_cast<size_t>(W + maxLag + 1));

    auto sample = [&](int64_t i) { return (i >= 0 && i < n) ? x[i] : 0.0f; };
    for (int64_t start = -W / 2; start + W / 2 < n; start += s.hop) {
        PitchFrame f;
        f.time = static_cast<double>(start + W / 2) / sr;
        // level
        double e = 0;
        for (int j = 0; j < W; ++j) e += static_cast<double>(sample(start + j)) * sample(start + j);
        f.rmsDb = gainToDb(std::sqrt(e / W));
        if (f.rmsDb < s.silenceDb) {
            track.frames.push_back(f);
            continue;
        }
        std::fill(a.begin(), a.end(), 0.0f);
        std::fill(b.begin(), b.end(), 0.0f);
        for (int j = 0; j < W; ++j) a[static_cast<size_t>(j)] = sample(start + j);
        for (int j = 0; j < W + maxLag; ++j) b[static_cast<size_t>(j)] = sample(start + j);
        fft.forwardReal(a.data(), A.data());
        fft.forwardReal(b.data(), B.data());
        for (size_t k = 0; k < A.size(); ++k) A[k] = std::conj(A[k]) * B[k];
        fft.inverseReal(A.data(), corr.data());
        // cumulative energies of b
        sq[0] = 0;
        for (int j = 0; j < W + maxLag; ++j) sq[static_cast<size_t>(j + 1)] = sq[static_cast<size_t>(j)] + static_cast<double>(b[static_cast<size_t>(j)]) * b[static_cast<size_t>(j)];
        const double e0 = sq[static_cast<size_t>(W)];
        for (int tau = 0; tau <= maxLag && tau < W + maxLag; ++tau) {
            const double et = sq[static_cast<size_t>(tau + W)] - sq[static_cast<size_t>(tau)];
            d[static_cast<size_t>(tau)] = std::max(0.0, e0 + et - 2.0 * corr[static_cast<size_t>(tau)]);
        }
        // cumulative mean normalized difference
        cmnd[0] = 1.0;
        double running = 0;
        for (int tau = 1; tau <= maxLag; ++tau) {
            running += d[static_cast<size_t>(tau)];
            cmnd[static_cast<size_t>(tau)] = running > 0 ? d[static_cast<size_t>(tau)] * tau / running : 1.0;
        }
        int best = -1;
        for (int tau = minLag; tau < maxLag; ++tau) {
            if (cmnd[static_cast<size_t>(tau)] < s.threshold) {
                while (tau + 1 < maxLag && cmnd[static_cast<size_t>(tau + 1)] < cmnd[static_cast<size_t>(tau)]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0) {
            // no dip under threshold: take the global minimum but mark low confidence
            double mv = 1e9;
            for (int tau = minLag; tau < maxLag; ++tau)
                if (cmnd[static_cast<size_t>(tau)] < mv) {
                    mv = cmnd[static_cast<size_t>(tau)];
                    best = tau;
                }
        }
        const double aper = cmnd[static_cast<size_t>(best)];
        double tauF = best;
        if (best > minLag && best + 1 < maxLag) {
            const double y0 = cmnd[static_cast<size_t>(best - 1)], y1 = cmnd[static_cast<size_t>(best)], y2 = cmnd[static_cast<size_t>(best + 1)];
            const double den = y0 - 2 * y1 + y2;
            if (std::fabs(den) > 1e-12) tauF = best + 0.5 * (y0 - y2) / den;
        }
        f.confidence = std::clamp(1.0 - aper, 0.0, 1.0);
        if (f.confidence >= s.minConfidence && tauF > 0) {
            f.hz = sr / tauF;
            f.midi = hzToMidi(f.hz);
            f.voiced = true;
        } else {
            f.confidence = 0; // reported as unvoiced / uncertain
        }
        track.frames.push_back(f);
    }
    // Octave-error cleanup: an isolated frame an octave away from both neighbours is corrected.
    for (size_t i = 1; i + 1 < track.frames.size(); ++i) {
        auto& f = track.frames[i];
        const auto& p = track.frames[i - 1];
        const auto& q = track.frames[i + 1];
        if (!f.voiced || !p.voiced || !q.voiced) continue;
        if (std::fabs(p.midi - q.midi) < 1.0) {
            for (double oct : {12.0, -12.0})
                if (std::fabs(f.midi + oct - p.midi) < 1.0) {
                    f.midi += oct;
                    f.hz = midiToHz(f.midi);
                }
        }
    }
    return track;
}

PitchTrack detectPitch(const std::vector<std::vector<float>>& ch, double sr, const PitchDetectorSettings& s) {
    if (ch.empty()) return {};
    if (ch.size() == 1) return detectPitch(ch[0].data(), static_cast<int64_t>(ch[0].size()), sr, s);
    std::vector<float> mono(ch[0].size());
    for (size_t i = 0; i < mono.size(); ++i) {
        float acc = 0;
        for (auto& c : ch) acc += c[i];
        mono[i] = acc / static_cast<float>(ch.size());
    }
    return detectPitch(mono.data(), static_cast<int64_t>(mono.size()), sr, s);
}

} // namespace roy::vocal
