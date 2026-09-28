#include "stems/StemSeparator.h"
#include "core/Files.h"
#include "core/Math.h"
#include "dsp/FFT.h"
#include "io/AudioFile.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::stems {

namespace fs = std::filesystem;

const Channels* StemResult::find(const std::string& name) const {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return &stems[i];
    return nullptr;
}

StemRegistry& StemRegistry::instance() {
    static StemRegistry r;
    return r;
}
void StemRegistry::add(std::unique_ptr<StemSeparator> s) { separators_[s->id()] = std::move(s); }
StemSeparator* StemRegistry::find(const std::string& id) const {
    auto it = separators_.find(id);
    return it == separators_.end() ? nullptr : it->second.get();
}
std::vector<StemSeparator*> StemRegistry::list() const {
    std::vector<StemSeparator*> v;
    for (auto& [k, s] : separators_) v.push_back(s.get());
    return v;
}

double sdr(const Channels& ref, const Channels& est) {
    double s = 0, e = 0;
    for (size_t c = 0; c < std::min(ref.size(), est.size()); ++c)
        for (size_t i = 0; i < std::min(ref[c].size(), est[c].size()); ++i) {
            s += static_cast<double>(ref[c][i]) * ref[c][i];
            const double d = static_cast<double>(ref[c][i]) - est[c][i];
            e += d * d;
        }
    return 10.0 * std::log10((s + 1e-20) / (e + 1e-20));
}

namespace {

class BasicDspSeparator : public StemSeparator {
public:
    std::string id() const override { return "roy.dsp-basic"; }
    std::string description() const override {
        return "Classic DSP separation (harmonic/percussive + band split + centre mask). Fast, no model, strong bleed.";
    }
    std::vector<std::string> stemNames() const override { return {"Vocals", "Drums", "Bass", "Other"}; }
    bool available(std::string*) const override { return true; }

    StemResult separate(const Channels& mixIn, double sr, const std::function<bool(double)>& progress) override {
        StemResult res;
        res.names = stemNames();
        Channels mix = mixIn;
        if (mix.size() == 1) mix.push_back(mix[0]);
        const size_t len = mix.empty() ? 0 : mix[0].size();
        const int N = 4096, H = 1024;
        dsp::FFT fft(N);
        const auto win = dsp::hannWindow(N);
        const size_t bins = N / 2 + 1;
        const size_t frames = len / H + N / H + 1;
        // STFT of L and R
        std::vector<std::vector<dsp::cpx>> X[2];
        for (int c = 0; c < 2; ++c) X[c].assign(frames, std::vector<dsp::cpx>(bins));
        std::vector<float> buf(N);
        for (size_t f = 0; f < frames; ++f) {
            const int64_t start = static_cast<int64_t>(f * H) - N;
            for (int c = 0; c < 2; ++c) {
                for (int i = 0; i < N; ++i) {
                    const int64_t k = start + i;
                    buf[static_cast<size_t>(i)] = (k >= 0 && k < static_cast<int64_t>(len) ? mix[static_cast<size_t>(c)][static_cast<size_t>(k)] : 0.0f) * win[static_cast<size_t>(i)];
                }
                fft.forwardReal(buf.data(), X[c][f].data());
            }
            if (progress && f % 64 == 0 && !progress(0.4 * static_cast<double>(f) / static_cast<double>(frames))) return res;
        }
        // magnitudes of mid and side
        std::vector<std::vector<float>> M(frames, std::vector<float>(bins)), S(frames, std::vector<float>(bins));
        for (size_t f = 0; f < frames; ++f)
            for (size_t b = 0; b < bins; ++b) {
                M[f][b] = std::abs((X[0][f][b] + X[1][f][b]) * 0.5f);
                S[f][b] = std::abs((X[0][f][b] - X[1][f][b]) * 0.5f);
            }
        // HPSS median filters (17 frames in time, 17 bins in frequency)
        const int half = 8;
        std::vector<std::vector<float>> Hm(frames, std::vector<float>(bins)), Pm(frames, std::vector<float>(bins));
        std::vector<float> w;
        for (size_t f = 0; f < frames; ++f) {
            for (size_t b = 0; b < bins; ++b) {
                w.clear();
                for (int k = -half; k <= half; ++k) {
                    const long ff = static_cast<long>(f) + k;
                    if (ff >= 0 && ff < static_cast<long>(frames)) w.push_back(M[static_cast<size_t>(ff)][b]);
                }
                std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
                Hm[f][b] = w[w.size() / 2];
                w.clear();
                for (int k = -half; k <= half; ++k) {
                    const long bb = static_cast<long>(b) + k;
                    if (bb >= 0 && bb < static_cast<long>(bins)) w.push_back(M[f][static_cast<size_t>(bb)]);
                }
                std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
                Pm[f][b] = w[w.size() / 2];
            }
            if (progress && f % 64 == 0 && !progress(0.4 + 0.3 * static_cast<double>(f) / static_cast<double>(frames))) return res;
        }
        // masks and inverse STFT
        res.stems.assign(4, Channels(2, std::vector<float>(len, 0.0f)));
        std::vector<std::vector<double>> acc(8, std::vector<double>(len + static_cast<size_t>(N), 0.0));
        std::vector<dsp::cpx> Y(bins);
        std::vector<float> y(N);
        std::vector<double> norm(len + static_cast<size_t>(N), 0.0);
        for (size_t f = 0; f < frames; ++f) {
            const int64_t start = static_cast<int64_t>(f * H) - N;
            for (int stem = 0; stem < 3; ++stem) {
                for (int c = 0; c < 2; ++c) {
                    for (size_t b = 0; b < bins; ++b) {
                        const double hz = static_cast<double>(b) * sr / N;
                        const double h2 = static_cast<double>(Hm[f][b]) * Hm[f][b], p2 = static_cast<double>(Pm[f][b]) * Pm[f][b];
                        const double harm = h2 / (h2 + p2 + 1e-18), perc = 1.0 - harm;
                        const double lowW = std::clamp((180.0 - hz) / 60.0, 0.0, 1.0); // bass crossover 120..180 Hz
                        const double m2 = static_cast<double>(M[f][b]) * M[f][b], s2 = static_cast<double>(S[f][b]) * S[f][b];
                        const double centre = m2 / (m2 + 4.0 * s2 + 1e-18);
                        const double vocalBand = hz > 150 && hz < 8000 ? 1.0 : 0.0;
                        double mask = 0;
                        if (stem == 0) mask = harm * (1.0 - lowW) * centre * vocalBand; // vocals
                        if (stem == 1) mask = perc * (1.0 - 0.7 * lowW);                  // drums (keep some kick low end)
                        if (stem == 2) mask = harm * lowW + perc * 0.7 * lowW * 0.0;      // bass: harmonic low band
                        Y[b] = X[c][f][b] * static_cast<float>(mask);
                    }
                    fft.inverseReal(Y.data(), y.data());
                    for (int i = 0; i < N; ++i) {
                        const int64_t k = start + i;
                        if (k < 0 || k >= static_cast<int64_t>(len)) continue;
                        acc[static_cast<size_t>(stem * 2 + c)][static_cast<size_t>(k)] += static_cast<double>(y[static_cast<size_t>(i)]) * win[static_cast<size_t>(i)];
                    }
                }
            }
            for (int i = 0; i < N; ++i) {
                const int64_t k = start + i;
                if (k >= 0 && k < static_cast<int64_t>(len)) norm[static_cast<size_t>(k)] += static_cast<double>(win[static_cast<size_t>(i)]) * win[static_cast<size_t>(i)];
            }
            if (progress && f % 64 == 0 && !progress(0.7 + 0.3 * static_cast<double>(f) / static_cast<double>(frames))) return res;
        }
        for (int stem = 0; stem < 3; ++stem)
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < len; ++i)
                    res.stems[static_cast<size_t>(stem)][static_cast<size_t>(c)][i] =
                        static_cast<float>(acc[static_cast<size_t>(stem * 2 + c)][i] / std::max(1e-9, norm[i]));
        // Other = residual -> the sum of all stems reproduces the input exactly.
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < len; ++i)
                res.stems[3][static_cast<size_t>(c)][i] = mix[static_cast<size_t>(c)][i] - res.stems[0][static_cast<size_t>(c)][i] -
                                                           res.stems[1][static_cast<size_t>(c)][i] - res.stems[2][static_cast<size_t>(c)][i];
        if (mixIn.size() == 1)
            for (auto& st : res.stems) st.resize(1);
        // quality report
        res.quality.method = id();
        double eIn = 0, eErr = 0;
        for (size_t c = 0; c < mixIn.size(); ++c)
            for (size_t i = 0; i < len; ++i) {
                double sum = 0;
                for (auto& st : res.stems) sum += st[c][i];
                eIn += static_cast<double>(mixIn[c][i]) * mixIn[c][i];
                eErr += (sum - mixIn[c][i]) * (sum - mixIn[c][i]);
            }
        res.quality.reconstructionErrorDb = 10 * std::log10((eErr + 1e-30) / (eIn + 1e-30));
        // cross-talk: correlation of 50 ms energy envelopes between stems
        const size_t hop = static_cast<size_t>(0.05 * sr);
        std::vector<std::vector<double>> env(4);
        for (size_t s = 0; s < 4; ++s)
            for (size_t i = 0; i + hop <= len; i += hop) {
                double e = 0;
                for (size_t k = i; k < i + hop; ++k) e += static_cast<double>(res.stems[s][0][k]) * res.stems[s][0][k];
                env[s].push_back(std::sqrt(e));
            }
        double ct = 0;
        int pairs = 0;
        for (size_t a = 0; a < 4; ++a)
            for (size_t b = a + 1; b < 4; ++b) {
                double ma = 0, mb = 0;
                const size_t n = env[a].size();
                if (n < 2) continue;
                for (size_t i = 0; i < n; ++i) { ma += env[a][i]; mb += env[b][i]; }
                ma /= static_cast<double>(n);
                mb /= static_cast<double>(n);
                double sab = 0, saa = 0, sbb = 0;
                for (size_t i = 0; i < n; ++i) {
                    sab += (env[a][i] - ma) * (env[b][i] - mb);
                    saa += (env[a][i] - ma) * (env[a][i] - ma);
                    sbb += (env[b][i] - mb) * (env[b][i] - mb);
                }
                if (saa > 0 && sbb > 0) {
                    ct += std::fabs(sab / std::sqrt(saa * sbb));
                    ++pairs;
                }
            }
        res.quality.crossTalk = pairs ? ct / pairs : 0.0;
        res.quality.warnings = {
            "Basic DSP separation: expect audible bleed between stems (e.g. sustained instruments in 'Vocals', kick in 'Bass').",
            "Centre-panned harmonic instruments are classified as vocals; wide stereo vocals may land in 'Other'.",
            "Percussive masks smear transients slightly (STFT 4096 / hop 1024).",
            "Sum of all stems reproduces the input exactly; the original file is not modified."};
        return res;
    }
};

} // namespace

void registerBuiltinSeparators() {
    static bool done = false;
    if (done) return;
    done = true;
    StemRegistry::instance().add(std::make_unique<BasicDspSeparator>());
}

std::vector<fs::path> writeStems(const StemResult& r, double sr, const fs::path& folder, const std::string& base, std::string* error) {
    std::vector<fs::path> out;
    for (size_t i = 0; i < r.stems.size(); ++i) {
        const fs::path p = files::uniquePath(folder / std::format("{}_{}.wav", base, r.names[i]));
        if (!writeWavFile(p, r.stems[i], sr, SampleFormat::Float32, false, error)) return {};
        out.push_back(p);
    }
    return out;
}

} // namespace roy::stems
