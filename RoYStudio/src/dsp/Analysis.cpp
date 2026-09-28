#include "dsp/Analysis.h"
#include "core/Math.h"
#include "dsp/FFT.h"
#include "dsp/Resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace roy::dsp {

std::vector<float> mixToMono(const std::vector<std::vector<float>>& ch) {
    if (ch.empty()) return {};
    if (ch.size() == 1) return ch[0];
    std::vector<float> m(ch[0].size());
    for (size_t i = 0; i < m.size(); ++i) {
        float a = 0;
        for (auto& c : ch) a += c[i];
        m[i] = a / static_cast<float>(ch.size());
    }
    return m;
}

double percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    const size_t k = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

std::vector<float> onsetFunction(const float* x, int64_t n, double, int fftSize, int hop) {
    FFT fft(fftSize);
    const int N = fft.size();
    auto win = hannWindow(N);
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<cpx> spec(static_cast<size_t>(N / 2 + 1));
    std::vector<float> prev(spec.size(), 0.0f), odf;
    for (int64_t start = -N / 2; start < n; start += hop) {
        for (int i = 0; i < N; ++i) {
            const int64_t k = start + i;
            frame[static_cast<size_t>(i)] = (k >= 0 && k < n ? x[k] : 0.0f) * win[static_cast<size_t>(i)];
        }
        fft.forwardReal(frame.data(), spec.data());
        double flux = 0;
        for (size_t b = 1; b < spec.size(); ++b) {
            const float mag = std::log1p(100.0f * std::abs(spec[b]));
            const float d = mag - prev[b];
            if (d > 0) flux += d;
            prev[b] = mag;
        }
        odf.push_back(static_cast<float>(flux));
    }
    return odf;
}

std::vector<Onset> detectOnsets(const float* x, int64_t n, double sr, const OnsetSettings& s) {
    std::vector<Onset> out;
    if (n <= 0) return out;
    auto odf = onsetFunction(x, n, sr, s.fftSize, s.hop);
    const float mx = *std::max_element(odf.begin(), odf.end());
    if (mx <= 1e-9f) return out;
    for (auto& v : odf) v /= mx;
    const int W = 12;
    const double delta = 0.06 / std::max(0.1, s.sensitivity);
    const double hopSec = static_cast<double>(s.hop) / sr;
    double last = -1e9;
    for (size_t i = 0; i + 1 < odf.size(); ++i) {
        const float v = odf[i];
        bool isMax = true;
        for (int k = -3; k <= 3 && isMax; ++k) {
            const long j = static_cast<long>(i) + k;
            if (k == 0 || j < 0 || j >= static_cast<long>(odf.size())) continue;
            if (odf[static_cast<size_t>(j)] > v) isMax = false;
        }
        if (!isMax) continue;
        std::vector<float> local;
        for (long j = static_cast<long>(i) - W; j <= static_cast<long>(i) + W; ++j)
            if (j >= 0 && j < static_cast<long>(odf.size())) local.push_back(odf[static_cast<size_t>(j)]);
        const double thr = percentile(local, 0.5) + delta;
        if (v < thr) continue;
        const double t = static_cast<double>(i) * hopSec; // frame centre (frames start at -N/2)
        if (t - last < s.minGapSeconds) continue;
        // A real onset raises the level: compare 25 ms before and after (rejects decays).
        {
            const int64_t c = static_cast<int64_t>(t * sr), w = static_cast<int64_t>(0.025 * sr);
            double eb = 1e-12, ea = 1e-12;
            for (int64_t k = std::max<int64_t>(0, c - w - s.hop / 2); k < std::max<int64_t>(0, c - s.hop / 2); ++k) eb += static_cast<double>(x[k]) * x[k];
            for (int64_t k = std::max<int64_t>(0, c - s.hop / 2); k < std::min(n, c + w); ++k) ea += static_cast<double>(x[k]) * x[k];
            if (10.0 * std::log10(ea / eb) < 3.0) continue;
        }
        last = t;
        out.push_back({t, v});
    }
    if (s.refine) {
        // Move each onset to the point where the fast envelope reaches 20 % of its local peak.
        const int64_t env = std::max<int64_t>(1, static_cast<int64_t>(sr * 0.001));
        for (auto& o : out) {
            const int64_t c = static_cast<int64_t>(o.time * sr);
            // The log-flux peak can precede the transient by up to half a frame: look ahead.
            const int64_t a = std::max<int64_t>(0, c - s.hop), b = std::min<int64_t>(n, c + s.fftSize / 2 + s.hop);
            std::vector<float> e;
            for (int64_t i = a; i < b; i += env) {
                float m = 0;
                for (int64_t k = i; k < std::min(b, i + env); ++k) m = std::max(m, std::fabs(x[k]));
                e.push_back(m);
            }
            if (e.empty()) continue;
            const float peak = *std::max_element(e.begin(), e.end());
            for (size_t k = 0; k < e.size(); ++k)
                if (e[k] >= 0.2f * peak) {
                    o.time = static_cast<double>(a + static_cast<int64_t>(k) * env) / sr;
                    break;
                }
        }
    }
    return out;
}

FrameLevels frameLevels(const float* x, int64_t n, double sr, double frameSec, double hopSec) {
    FrameLevels f;
    f.hopSeconds = hopSec;
    const int64_t W = std::max<int64_t>(1, static_cast<int64_t>(frameSec * sr));
    const int64_t H = std::max<int64_t>(1, static_cast<int64_t>(hopSec * sr));
    for (int64_t s = 0; s < n; s += H) {
        double e = 0;
        float pk = 0;
        const int64_t end = std::min(n, s + W);
        for (int64_t i = s; i < end; ++i) {
            e += static_cast<double>(x[i]) * x[i];
            pk = std::max(pk, std::fabs(x[i]));
        }
        f.rmsDb.push_back(static_cast<float>(gainToDb(std::sqrt(e / static_cast<double>(std::max<int64_t>(1, end - s))))));
        f.peakDb.push_back(static_cast<float>(gainToDb(static_cast<double>(pk))));
    }
    return f;
}

std::vector<float> bandEnergy(const float* x, int64_t n, double sr, double lo, double hi, int fftSize, int hop) {
    FFT fft(fftSize);
    const int N = fft.size();
    auto win = hannWindow(N);
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<cpx> spec(static_cast<size_t>(N / 2 + 1));
    std::vector<float> out;
    const int b0 = std::max(0, static_cast<int>(std::floor(lo * N / sr)));
    const int b1 = std::min(N / 2, static_cast<int>(std::ceil(hi * N / sr)));
    for (int64_t start = 0; start < n; start += hop) {
        for (int i = 0; i < N; ++i) {
            const int64_t k = start + i;
            frame[static_cast<size_t>(i)] = (k < n ? x[k] : 0.0f) * win[static_cast<size_t>(i)];
        }
        fft.forwardReal(frame.data(), spec.data());
        double e = 0;
        for (int b = b0; b < b1; ++b) e += std::norm(spec[static_cast<size_t>(b)]);
        out.push_back(static_cast<float>(e / (N * 0.375 * N)));
    }
    return out;
}

std::vector<float> averageSpectrumDb(const float* x, int64_t n, double, int fftSize) {
    FFT fft(fftSize);
    const int N = fft.size();
    auto win = hannWindow(N);
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<cpx> spec(static_cast<size_t>(N / 2 + 1));
    std::vector<double> acc(spec.size(), 0.0);
    int frames = 0;
    for (int64_t start = 0; start + N <= std::max<int64_t>(n, N); start += N / 2) {
        for (int i = 0; i < N; ++i) {
            const int64_t k = start + i;
            frame[static_cast<size_t>(i)] = (k < n ? x[k] : 0.0f) * win[static_cast<size_t>(i)];
        }
        fft.forwardReal(frame.data(), spec.data());
        for (size_t b = 0; b < spec.size(); ++b) acc[b] += std::norm(spec[b]);
        ++frames;
        if (start + N >= n) break;
    }
    std::vector<float> out(spec.size());
    for (size_t b = 0; b < out.size(); ++b)
        out[b] = static_cast<float>(10.0 * std::log10(acc[b] / std::max(1, frames) / (N * 0.375 * N) + 1e-20));
    return out;
}

std::vector<double> estimateFormants(const float* x, int64_t n, double sr, int count) {
    std::vector<double> out;
    if (n < 64) return out;
    const double target = 11025.0;
    std::vector<float> seg(x, x + n);
    std::vector<float> y = resample(seg, sr, target, 16);
    const int order = 12;
    if (y.size() < static_cast<size_t>(order * 4)) return out;
    // pre-emphasis + Hamming
    for (size_t i = y.size() - 1; i > 0; --i) y[i] -= 0.97f * y[i - 1];
    for (size_t i = 0; i < y.size(); ++i)
        y[i] *= static_cast<float>(0.54 - 0.46 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(y.size() - 1)));
    std::vector<double> r(order + 1, 0.0);
    for (int k = 0; k <= order; ++k)
        for (size_t i = static_cast<size_t>(k); i < y.size(); ++i) r[static_cast<size_t>(k)] += static_cast<double>(y[i]) * y[i - static_cast<size_t>(k)];
    if (r[0] <= 1e-12) return out;
    r[0] *= 1.0 + 1e-9;
    // Levinson-Durbin
    std::vector<double> a(order + 1, 0.0), tmp(order + 1);
    a[0] = 1.0;
    double err = r[0];
    for (int i = 1; i <= order; ++i) {
        double acc = r[static_cast<size_t>(i)];
        for (int j = 1; j < i; ++j) acc += a[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];
        const double k = -acc / err;
        tmp = a;
        for (int j = 1; j < i; ++j) a[static_cast<size_t>(j)] = tmp[static_cast<size_t>(j)] + k * tmp[static_cast<size_t>(i - j)];
        a[static_cast<size_t>(i)] = k;
        err *= 1.0 - k * k;
        if (err <= 0) break;
    }
    // envelope peaks
    const int P = 1024;
    std::vector<double> env(P);
    for (int q = 0; q < P; ++q) {
        const double w = kPi * q / P; // 0..nyquist(5512 Hz)
        std::complex<double> A(0, 0);
        for (int j = 0; j <= order; ++j) A += a[static_cast<size_t>(j)] * std::polar(1.0, -w * j);
        env[static_cast<size_t>(q)] = 1.0 / std::max(1e-12, std::abs(A));
    }
    for (int q = 1; q + 1 < P && static_cast<int>(out.size()) < count; ++q) {
        if (env[static_cast<size_t>(q)] > env[static_cast<size_t>(q - 1)] && env[static_cast<size_t>(q)] >= env[static_cast<size_t>(q + 1)]) {
            const double hz = static_cast<double>(q) / P * target / 2.0;
            if (hz > 150.0) out.push_back(hz);
        }
    }
    return out;
}

std::vector<std::vector<float>> alignmentFeatures(const float* x, int64_t n, double sr, int hop) {
    const int N = 1024;
    FFT fft(N);
    auto win = hannWindow(N);
    std::vector<float> frame(N);
    std::vector<cpx> spec(N / 2 + 1);
    std::vector<float> prev(spec.size(), 0.0f);
    std::vector<std::vector<float>> f;
    for (int64_t start = -N / 2; start < n; start += hop) {
        double e = 0, cnum = 0, cden = 0, flux = 0;
        for (int i = 0; i < N; ++i) {
            const int64_t k = start + i;
            const float v = k >= 0 && k < n ? x[k] : 0.0f;
            frame[static_cast<size_t>(i)] = v * win[static_cast<size_t>(i)];
            e += static_cast<double>(v) * v;
        }
        fft.forwardReal(frame.data(), spec.data());
        for (size_t b = 1; b < spec.size(); ++b) {
            const float m = std::abs(spec[b]);
            cnum += static_cast<double>(b) * m;
            cden += m;
            const float lm = std::log1p(100.0f * m);
            if (lm > prev[b]) flux += lm - prev[b];
            prev[b] = lm;
        }
        f.push_back({static_cast<float>(10.0 * std::log10(e / N + 1e-10)), static_cast<float>(flux),
                     static_cast<float>(cden > 0 ? cnum / cden * sr / N / 1000.0 : 0.0)});
    }
    // z-score each dimension
    for (size_t d = 0; d < 3; ++d) {
        double mean = 0, var = 0;
        for (auto& v : f) mean += v[d];
        mean /= std::max<size_t>(1, f.size());
        for (auto& v : f) var += (v[d] - mean) * (v[d] - mean);
        const double sd = std::sqrt(var / std::max<size_t>(1, f.size())) + 1e-6;
        for (auto& v : f) v[d] = static_cast<float>((v[d] - mean) / sd);
    }
    return f;
}

std::vector<std::pair<int, int>> dtwAlign(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b, int band) {
    const int na = static_cast<int>(a.size()), nb = static_cast<int>(b.size());
    std::vector<std::pair<int, int>> path;
    if (na == 0 || nb == 0) return path;
    band = std::max(band, std::abs(na - nb) + 2);
    const int W = 2 * band + 1;
    const float INF = std::numeric_limits<float>::infinity();
    std::vector<float> cost(static_cast<size_t>(na) * W, INF);
    std::vector<uint8_t> dir(static_cast<size_t>(na) * W, 0);
    auto centre = [&](int i) { return static_cast<int>(std::llround(static_cast<double>(i) * (nb - 1) / std::max(1, na - 1))); };
    auto at = [&](int i, int j) -> float& { return cost[static_cast<size_t>(i) * W + static_cast<size_t>(j - centre(i) + band)]; };
    auto inBand = [&](int i, int j) { return j >= 0 && j < nb && std::abs(j - centre(i)) <= band; };
    auto dist = [&](int i, int j) {
        float d = 0;
        for (size_t k = 0; k < a[static_cast<size_t>(i)].size(); ++k) {
            const float q = a[static_cast<size_t>(i)][k] - b[static_cast<size_t>(j)][k];
            d += q * q;
        }
        return std::sqrt(d);
    };
    for (int i = 0; i < na; ++i) {
        for (int j = std::max(0, centre(i) - band); j <= std::min(nb - 1, centre(i) + band); ++j) {
            const float d = dist(i, j);
            float best = INF;
            uint8_t bd = 0;
            if (i == 0 && j == 0) {
                best = 0;
            } else {
                if (i > 0 && j > 0 && inBand(i - 1, j - 1) && at(i - 1, j - 1) < best) { best = at(i - 1, j - 1); bd = 1; }
                if (i > 0 && inBand(i - 1, j) && at(i - 1, j) + 0.3f < best) { best = at(i - 1, j) + 0.3f; bd = 2; }
                if (j > 0 && inBand(i, j - 1) && at(i, j - 1) + 0.3f < best) { best = at(i, j - 1) + 0.3f; bd = 3; }
            }
            at(i, j) = best + d;
            dir[static_cast<size_t>(i) * W + static_cast<size_t>(j - centre(i) + band)] = bd;
        }
    }
    int i = na - 1, j = nb - 1;
    if (!inBand(i, j)) return path;
    while (i >= 0 && j >= 0) {
        path.push_back({i, j});
        const uint8_t d = dir[static_cast<size_t>(i) * W + static_cast<size_t>(j - centre(i) + band)];
        if (d == 0) break;
        if (d == 1) { --i; --j; }
        else if (d == 2) --i;
        else --j;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

TempoEstimate estimateBpm(const float* x, int64_t n, double sr, double minBpm, double maxBpm) {
    TempoEstimate t;
    const int hop = 256;
    auto odf = onsetFunction(x, n, sr, 1024, hop);
    if (odf.size() < 16) return t;
    const double mean = std::accumulate(odf.begin(), odf.end(), 0.0) / static_cast<double>(odf.size());
    for (auto& v : odf) v = std::max(0.0f, v - static_cast<float>(mean));
    const double fps = sr / hop;
    const int maxLag = static_cast<int>(fps * 60.0 / minBpm * 2) + 2;
    std::vector<double> acf(static_cast<size_t>(maxLag + 1), 0.0);
    for (int lag = 1; lag <= maxLag && lag < static_cast<int>(odf.size()); ++lag) {
        double s = 0;
        for (size_t i = static_cast<size_t>(lag); i < odf.size(); ++i) s += static_cast<double>(odf[i]) * odf[i - static_cast<size_t>(lag)];
        acf[static_cast<size_t>(lag)] = s / static_cast<double>(odf.size() - static_cast<size_t>(lag));
    }
    auto acfAt = [&](double lag) {
        const size_t i = static_cast<size_t>(lag);
        if (i + 1 >= acf.size()) return 0.0;
        const double f = lag - static_cast<double>(i);
        return acf[i] * (1 - f) + acf[i + 1] * f;
    };
    double best = -1, bestBpm = 0, sum = 0;
    int count = 0;
    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.1) {
        const double lag = fps * 60.0 / bpm;
        const double score = acfAt(lag) + 0.5 * acfAt(2 * lag) + 0.25 * acfAt(lag / 2);
        sum += score;
        ++count;
        if (score > best) {
            best = score;
            bestBpm = bpm;
        }
    }
    t.bpm = std::round(bestBpm * 10.0) / 10.0;
    t.confidence = count && sum > 0 ? std::clamp((best / (sum / count) - 1.0) / 3.0, 0.0, 1.0) : 0.0;
    return t;
}

KeyEstimate estimateKey(const float* x, int64_t n, double sr) {
    KeyEstimate k;
    const int N = 8192;
    FFT fft(N);
    auto win = hannWindow(N);
    std::vector<float> frame(N);
    std::vector<cpx> spec(N / 2 + 1);
    std::vector<double> chroma(12, 0.0);
    for (int64_t start = 0; start < n; start += N / 2) {
        for (int i = 0; i < N; ++i) {
            const int64_t q = start + i;
            frame[static_cast<size_t>(i)] = (q < n ? x[q] : 0.0f) * win[static_cast<size_t>(i)];
        }
        fft.forwardReal(frame.data(), spec.data());
        for (int b = 1; b <= N / 2; ++b) {
            const double hz = static_cast<double>(b) * sr / N;
            if (hz < 55 || hz > 4000) continue;
            const double m = std::abs(spec[static_cast<size_t>(b)]);
            const int pc = static_cast<int>(std::lround(hzToMidi(hz))) % 12;
            chroma[static_cast<size_t>((pc + 12) % 12)] += m;
        }
    }
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    auto corr = [&](const double* prof, int root) {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) {
            mx += chroma[static_cast<size_t>(i)];
            my += prof[(i - root + 12) % 12];
        }
        mx /= 12;
        my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) {
            const double dx = chroma[static_cast<size_t>(i)] - mx, dy = prof[(i - root + 12) % 12] - my;
            sxy += dx * dy;
            sxx += dx * dx;
            syy += dy * dy;
        }
        return sxy / std::sqrt(sxx * syy + 1e-12);
    };
    double best = -2, second = -2;
    for (int r = 0; r < 12; ++r)
        for (int m = 0; m < 2; ++m) {
            const double c = corr(m ? minor : major, r);
            if (c > best) {
                second = best;
                best = c;
                k.root = r;
                k.minor = m == 1;
            } else if (c > second) {
                second = c;
            }
        }
    k.confidence = std::clamp(best - second, 0.0, 1.0);
    const double mxv = *std::max_element(chroma.begin(), chroma.end());
    for (double c : chroma) k.chroma.push_back(static_cast<float>(mxv > 0 ? c / mxv : 0));
    return k;
}

} // namespace roy::dsp
