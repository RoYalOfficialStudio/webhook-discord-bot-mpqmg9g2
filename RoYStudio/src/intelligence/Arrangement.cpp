#include "intelligence/Arrangement.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"
#include "dsp/Loudness.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

namespace roy::arrangei {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool roleHas(const std::string& role, const std::string& name, std::initializer_list<const char*> words) {
    const std::string r = lower(role + " " + name);
    for (auto* w : words)
        if (r.find(w) != std::string::npos) return true;
    return false;
}
double energyOf(const std::vector<float>& x, size_t a, size_t b) {
    double e = 0;
    for (size_t i = a; i < std::min(b, x.size()); ++i) e += static_cast<double>(x[i]) * x[i];
    return e;
}
} // namespace

EnergyMap buildEnergyMap(const mixi::Channels& master, const std::vector<mixi::TrackAudio>& tracks, double sr, const TempoMap& tempo,
                         double startBeat) {
    EnergyMap map;
    if (master.empty() || master[0].empty()) return map;
    const auto mono = dsp::mixToMono(master);
    const size_t n = mono.size();
    // low band for bass activity
    dsp::Biquad lp1, lp2;
    lp1.set(dsp::Biquad::Type::LowPass, sr, 120.0, 0.5412);
    lp2.set(dsp::Biquad::Type::LowPass, sr, 120.0, 1.3066);
    std::vector<float> low(n);
    for (size_t i = 0; i < n; ++i) low[i] = lp2.process(lp1.process(mono[i]));
    auto onsets = dsp::detectOnsets(mono.data(), static_cast<int64_t>(n), sr);
    std::vector<std::vector<float>> drums, vocals;
    for (auto& t : tracks) {
        if (roleHas(t.role, t.name, {"drum", "kick", "snare", "hat", "perc", "beat"})) drums.push_back(dsp::mixToMono(t.audio));
        if (roleHas(t.role, t.name, {"vocal", "vox", "adlib", "rap"})) vocals.push_back(dsp::mixToMono(t.audio));
    }
    const double startSec = tempo.beatToSeconds(startBeat);
    dsp::FFT fft(4096);
    for (int bar = tempo.beatToBarBeat(startBeat).bar;; ++bar) {
        const double b0 = std::max(startBeat, tempo.barToBeat(bar)), b1 = tempo.barToBeat(bar + 1);
        const size_t s0 = static_cast<size_t>(std::max(0.0, (tempo.beatToSeconds(b0) - startSec) * sr));
        const size_t s1 = static_cast<size_t>(std::max(0.0, (tempo.beatToSeconds(b1) - startSec) * sr));
        if (s0 >= n || s1 <= s0) break;
        const size_t e1 = std::min(s1, n);
        EnergyFrame f;
        f.startBeat = b0;
        f.endBeat = b1;
        std::vector<std::vector<float>> seg(master.size());
        for (size_t c = 0; c < master.size(); ++c) seg[c].assign(master[c].begin() + static_cast<long>(s0), master[c].begin() + static_cast<long>(e1));
        f.loudnessLufs = dsp::measureLoudness(seg, sr).integratedLufs;
        if (f.loudnessLufs <= -70.0) f.loudnessLufs = gainToDb(std::sqrt(energyOf(mono, s0, e1) / static_cast<double>(e1 - s0) + 1e-20)) - 0.7;
        const double secs = static_cast<double>(e1 - s0) / sr;
        int cnt = 0;
        for (auto& o : onsets)
            if (o.time * sr >= static_cast<double>(s0) && o.time * sr < static_cast<double>(e1)) ++cnt;
        f.density = cnt / std::max(1e-9, secs);
        // spectral centroid of the bar (averaged spectrum)
        const auto spec = dsp::averageSpectrumDb(mono.data() + s0, static_cast<int64_t>(e1 - s0), sr, 4096);
        double num = 0, den = 0;
        for (size_t k = 1; k < spec.size(); ++k) {
            const double p = std::pow(10.0, spec[k] / 10.0);
            num += p * static_cast<double>(k) * sr / 4096.0;
            den += p;
        }
        f.spectralCentroid = den > 0 ? num / den : 0;
        const double eAll = energyOf(mono, s0, e1) + 1e-20;
        f.bassActivity = std::clamp(energyOf(low, s0, e1) / eAll, 0.0, 1.0);
        if (!drums.empty()) {
            double ed = 0;
            for (auto& d : drums) ed += energyOf(d, s0, e1);
            f.drumActivity = std::clamp(ed / eAll, 0.0, 1.0);
        } else {
            f.drumActivity = std::clamp(f.density / 8.0, 0.0, 1.0); // percussive estimate from onset density
        }
        if (!vocals.empty()) {
            size_t active = 0, total = 0;
            const size_t w = static_cast<size_t>(0.05 * sr);
            for (size_t i = s0; i + w <= e1; i += w) {
                double ev = 0;
                for (auto& v : vocals) ev += energyOf(v, i, i + w);
                if (std::sqrt(ev / static_cast<double>(w)) > dbToGain(-45.0)) ++active;
                ++total;
            }
            f.vocalActivity = total ? static_cast<double>(active) / static_cast<double>(total) : 0.0;
        }
        map.bars.push_back(f);
        if (s1 >= n) break;
    }
    // combined energy, normalised over the song
    if (!map.bars.empty()) {
        double lmin = 1e9, lmax = -1e9, dmax = 1e-9, cmax = 1e-9;
        for (auto& b : map.bars) {
            lmin = std::min(lmin, b.loudnessLufs);
            lmax = std::max(lmax, b.loudnessLufs);
            dmax = std::max(dmax, b.density);
            cmax = std::max(cmax, b.spectralCentroid);
        }
        for (auto& b : map.bars) {
            const double l = lmax > lmin ? (b.loudnessLufs - lmin) / (lmax - lmin) : 0.5;
            b.energy = std::clamp(0.5 * l + 0.2 * b.density / dmax + 0.1 * b.spectralCentroid / cmax + 0.1 * b.drumActivity + 0.1 * b.vocalActivity, 0.0, 1.0);
        }
    }
    return map;
}

std::vector<SectionSuggestion> suggestSections(const EnergyMap& map, int minBars) {
    std::vector<SectionSuggestion> out;
    const size_t n = map.bars.size();
    if (n == 0) return out;
    // feature vectors per bar (z-scored)
    std::vector<std::vector<double>> F(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& b = map.bars[i];
        F[i] = {b.loudnessLufs, b.density, b.spectralCentroid / 1000.0, b.drumActivity * 10, b.bassActivity * 10, b.vocalActivity * 10};
    }
    for (size_t d = 0; d < F[0].size(); ++d) {
        double m = 0, v = 0;
        for (auto& f : F) m += f[d];
        m /= static_cast<double>(n);
        for (auto& f : F) v += (f[d] - m) * (f[d] - m);
        const double sd = std::sqrt(v / static_cast<double>(n)) + 1e-6;
        for (auto& f : F) f[d] = (f[d] - m) / sd;
    }
    auto dist = [&](size_t a, size_t b) {
        double s = 0;
        for (size_t d = 0; d < F[a].size(); ++d) s += (F[a][d] - F[b][d]) * (F[a][d] - F[b][d]);
        return std::sqrt(s);
    };
    // novelty: distance between the mean of the previous and next `k` bars
    const int k = std::max(2, minBars / 2);
    std::vector<double> nov(n, 0.0);
    for (size_t i = static_cast<size_t>(k); i + static_cast<size_t>(k) <= n; ++i) {
        std::vector<double> a(F[0].size(), 0.0), b(F[0].size(), 0.0);
        for (int j = 0; j < k; ++j)
            for (size_t d = 0; d < a.size(); ++d) {
                a[d] += F[i - 1 - static_cast<size_t>(j)][d] / k;
                b[d] += F[i + static_cast<size_t>(j)][d] / k;
            }
        double s = 0;
        for (size_t d = 0; d < a.size(); ++d) s += (a[d] - b[d]) * (a[d] - b[d]);
        nov[i] = std::sqrt(s);
    }
    // boundaries: novelty peaks above the median, at least minBars apart
    std::vector<size_t> bounds = {0};
    std::vector<double> sorted = nov;
    std::sort(sorted.begin(), sorted.end());
    const double thr = sorted[sorted.size() / 2] + 0.5;
    for (size_t i = 1; i + 1 < n; ++i) {
        if (nov[i] < thr || nov[i] < nov[i - 1] || nov[i] < nov[i + 1]) continue;
        if (static_cast<int>(i - bounds.back()) < minBars) {
            if (nov[i] > nov[bounds.back()] && bounds.size() > 1) bounds.back() = i;
            continue;
        }
        if (static_cast<int>(n - i) < minBars) continue;
        bounds.push_back(i);
    }
    bounds.push_back(n);
    // segment means + clustering by similarity
    struct Seg { size_t a, b; std::vector<double> mean; double energy; int cluster; };
    std::vector<Seg> segs;
    for (size_t s = 0; s + 1 < bounds.size(); ++s) {
        Seg g{bounds[s], bounds[s + 1], std::vector<double>(F[0].size(), 0.0), 0.0, -1};
        for (size_t i = g.a; i < g.b; ++i) {
            for (size_t d = 0; d < g.mean.size(); ++d) g.mean[d] += F[i][d] / static_cast<double>(g.b - g.a);
            g.energy += map.bars[i].energy / static_cast<double>(g.b - g.a);
        }
        segs.push_back(g);
    }
    int clusters = 0;
    for (auto& g : segs) {
        for (auto& h : segs) {
            if (&h == &g || h.cluster < 0) continue;
            double s = 0;
            for (size_t d = 0; d < g.mean.size(); ++d) s += (g.mean[d] - h.mean[d]) * (g.mean[d] - h.mean[d]);
            if (std::sqrt(s) < 1.0) {
                g.cluster = h.cluster;
                break;
            }
        }
        if (g.cluster < 0) g.cluster = clusters++;
    }
    (void)dist;
    // labels
    std::map<int, int> clusterCount;
    std::map<int, double> clusterEnergy;
    for (auto& g : segs) {
        clusterCount[g.cluster]++;
        clusterEnergy[g.cluster] = std::max(clusterEnergy[g.cluster], g.energy);
    }
    int hookCluster = -1;
    double best = -1;
    for (auto& [c, e] : clusterEnergy)
        if (e > best && (clusterCount[c] > 1 || segs.size() <= 3)) {
            best = e;
            hookCluster = c;
        }
    std::map<std::string, int> counters;
    for (size_t s = 0; s < segs.size(); ++s) {
        const auto& g = segs[s];
        SectionSuggestion sg;
        sg.startBeat = map.bars[g.a].startBeat;
        sg.endBeat = map.bars[g.b - 1].endBeat;
        sg.cluster = g.cluster;
        sg.meanEnergy = g.energy;
        const bool first = s == 0, last = s + 1 == segs.size();
        if (g.cluster == hookCluster) sg.type = "hook";
        else if (first && g.energy < 0.5) sg.type = "intro";
        else if (last && g.energy < 0.5) sg.type = "outro";
        else if (s + 1 < segs.size() && segs[s + 1].cluster == hookCluster && g.energy < segs[s + 1].energy && (g.b - g.a) <= 4)
            sg.type = "pre_hook";
        else if (clusterCount[g.cluster] == 1 && !first && !last) sg.type = "bridge";
        else sg.type = "verse";
        const int num = ++counters[sg.type];
        static const std::map<std::string, std::string> names = {{"intro", "Intro"}, {"verse", "Verse"}, {"pre_hook", "Pre-Hook"},
                                                                 {"hook", "Hook"}, {"bridge", "Bridge"}, {"outro", "Outro"}};
        sg.name = (sg.type == "intro" || sg.type == "outro") ? names.at(sg.type) : std::format("{} {}", names.at(sg.type), num);
        sg.confidence = std::clamp(0.4 + 0.3 * (clusterCount[g.cluster] > 1 ? 1.0 : 0.0) + 0.3 * std::min(1.0, nov[g.a] / 3.0), 0.0, 1.0);
        out.push_back(sg);
    }
    return out;
}

} // namespace roy::arrangei
