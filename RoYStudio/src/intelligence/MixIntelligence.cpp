#include "intelligence/MixIntelligence.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "dsp/FFT.h"
#include "dsp/Filters.h"
#include "dsp/Loudness.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace roy::mixi {

const std::vector<double>& thirdOctaveCentres() {
    static const std::vector<double> c = {25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                                          1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000};
    return c;
}

nlohmann::json MixReport::toJson() const {
    nlohmann::json j;
    j["master"] = {{"peakDb", masterPeakDb}, {"truePeakDb", masterTruePeakDb}, {"lufs", masterLufs}, {"correlation", masterCorrelation},
                   {"lowSideRatioDb", masterLowSideRatioDb}};
    j["issues"] = nlohmann::json::array();
    for (auto& i : issues) {
        nlohmann::json sug = nlohmann::json::array();
        for (auto& s : i.suggestions) sug.push_back({{"description", s.description}, {"command", s.command}, {"args", s.args}});
        j["issues"].push_back({{"type", i.type}, {"severity", i.severity}, {"tracks", i.tracks}, {"lowHz", i.lowHz}, {"highHz", i.highHz},
                               {"title", i.title}, {"detail", i.detail}, {"value", i.value}, {"suggestions", sug}});
    }
    return j;
}

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string hzText(double hz) { return hz >= 1000 ? std::format("{:.1f} kHz", hz / 1000.0) : std::format("{:.0f} Hz", hz); }

// [frame][band] energies in dB of a mono signal.
std::vector<std::vector<float>> bandMatrix(const std::vector<float>& x, double sr, int N, int hop) {
    dsp::FFT fft(N);
    const auto win = dsp::hannWindow(N);
    const auto& centres = thirdOctaveCentres();
    std::vector<std::pair<int, int>> edges;
    for (double c : centres) {
        const int b0 = std::max(1, static_cast<int>(std::floor(c / std::pow(2.0, 1.0 / 6.0) * N / sr)));
        const int b1 = std::max(b0 + 1, static_cast<int>(std::ceil(c * std::pow(2.0, 1.0 / 6.0) * N / sr)));
        edges.push_back({b0, std::min(b1, N / 2)});
    }
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<dsp::cpx> spec(static_cast<size_t>(N / 2 + 1));
    std::vector<std::vector<float>> m;
    for (size_t start = 0; start + static_cast<size_t>(N) <= x.size(); start += static_cast<size_t>(hop)) {
        for (int i = 0; i < N; ++i) frame[static_cast<size_t>(i)] = x[start + static_cast<size_t>(i)] * win[static_cast<size_t>(i)];
        fft.forwardReal(frame.data(), spec.data());
        std::vector<float> row(centres.size());
        for (size_t k = 0; k < centres.size(); ++k) {
            double e = 0;
            for (int b = edges[k].first; b < edges[k].second; ++b) e += std::norm(spec[static_cast<size_t>(b)]);
            row[k] = static_cast<float>(10.0 * std::log10(e / (N * 0.375 * N) + 1e-20));
        }
        m.push_back(std::move(row));
    }
    return m;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 1.0;
}

std::vector<float> lowPassed(const std::vector<float>& x, double sr, double hz) {
    dsp::Biquad a, b;
    a.set(dsp::Biquad::Type::LowPass, sr, hz, 0.5412);
    b.set(dsp::Biquad::Type::LowPass, sr, hz, 1.3066);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = b.process(a.process(x[i]));
    return y;
}

bool isVocal(const std::string& role) {
    const std::string r = upper(role);
    return r.find("VOCAL") != std::string::npos || r == "ADLIB" || r == "DOUBLE" || r == "HARMONY" || r == "VOX";
}

} // namespace

MixReport analyzeMix(const std::vector<TrackAudio>& tracks, const Channels& master, const MixSettings& s) {
    MixReport rep;
    const double sr = s.sampleRate;
    const int N = 4096, hop = 2048;
    const auto& centres = thirdOctaveCentres();

    std::vector<std::vector<float>> mono(tracks.size());
    std::vector<std::vector<std::vector<float>>> bands(tracks.size());
    for (size_t t = 0; t < tracks.size(); ++t) {
        mono[t] = dsp::mixToMono(tracks[t].audio);
        bands[t] = bandMatrix(mono[t], sr, N, hop);
    }

    // ---- clipping ----
    for (size_t t = 0; t < tracks.size(); ++t) {
        float pk = 0;
        for (auto& c : tracks[t].audio)
            for (float v : c) pk = std::max(pk, std::fabs(v));
        if (pk > 1.0f) {
            MixIssue i;
            i.type = "clipping";
            i.severity = "warning";
            i.tracks = {tracks[t].name};
            i.value = gainToDb(static_cast<double>(pk));
            i.title = std::format("{} OUTPUT OVER 0 dBFS", upper(tracks[t].name));
            i.detail = std::format("track output peaks at {:+.1f} dBFS (floating point, clips when bounced/exported without headroom)", i.value);
            i.suggestions.push_back({std::format("Lower the {} fader by {:.1f} dB", tracks[t].name, i.value + 1.0), "SetChannelGain",
                                     {{"channelId", tracks[t].channelId}, {"gainDb", -(i.value + 1.0)}, {"relative", true}}});
            rep.issues.push_back(i);
        }
    }

    // ---- masking (target <- masker) ----
    struct MaskHit { size_t target, masker; size_t b0, b1; double fraction; double weight; };
    std::vector<MaskHit> hits;
    for (size_t T = 0; T < tracks.size(); ++T)
        for (size_t M = 0; M < tracks.size(); ++M) {
            if (T == M) continue;
            const auto& bt = bands[T];
            const auto& bm = bands[M];
            const size_t frames = std::min(bt.size(), bm.size());
            if (frames == 0) continue;
            std::vector<double> frac(centres.size(), 0.0), act(centres.size(), 0.0);
            // "present" = within 20 dB of the target's loudest band (filter skirts do not count)
            float mx = -200;
            for (size_t f = 0; f < frames; ++f)
                for (size_t k = 0; k < centres.size(); ++k) mx = std::max(mx, bt[f][k]);
            for (size_t k = 0; k < centres.size(); ++k) {
                int active = 0, masked = 0;
                for (size_t f = 0; f < frames; ++f) {
                    if (bt[f][k] < -60.0f || bt[f][k] < mx - 20.0f) continue;
                    ++active;
                    if (bm[f][k] >= bt[f][k] - s.maskingMarginDb) ++masked;
                }
                act[k] = static_cast<double>(active) / static_cast<double>(frames);
                frac[k] = active ? static_cast<double>(masked) / active : 0.0;
            }
            // contiguous runs of masked bands (the target must be present in >= 20 % of frames)
            for (size_t k = 0; k < centres.size();) {
                if (!(frac[k] >= s.maskingMinFraction && act[k] >= 0.2)) { ++k; continue; }
                size_t e = k;
                double fs = 0, ws = 0;
                while (e < centres.size() && frac[e] >= s.maskingMinFraction && act[e] >= 0.2) {
                    fs += frac[e];
                    ws += act[e];
                    ++e;
                }
                if (e - k >= 2) hits.push_back({T, M, k, e - 1, fs / static_cast<double>(e - k), ws});
                k = e;
            }
        }
    // Keep the most relevant hits: vocals first, then by masked fraction x width.
    std::sort(hits.begin(), hits.end(), [&](const MaskHit& a, const MaskHit& b) {
        const bool va = isVocal(tracks[a.target].role), vb = isVocal(tracks[b.target].role);
        if (va != vb) return va;
        return a.fraction * static_cast<double>(a.b1 - a.b0 + 1) > b.fraction * static_cast<double>(b.b1 - b.b0 + 1);
    });
    std::vector<std::pair<size_t, size_t>> reported;
    for (auto& h : hits) {
        const std::pair<size_t, size_t> key{std::min(h.target, h.masker), std::max(h.target, h.masker)};
        if (std::find(reported.begin(), reported.end(), key) != reported.end()) continue;
        if (reported.size() >= 8) break;
        reported.push_back(key);
        MixIssue i;
        i.type = "masking";
        i.severity = h.fraction > 0.6 ? "warning" : "info";
        i.tracks = {tracks[h.target].name, tracks[h.masker].name};
        i.lowHz = centres[h.b0] / std::pow(2.0, 1.0 / 6.0);
        i.highHz = centres[h.b1] * std::pow(2.0, 1.0 / 6.0);
        i.value = h.fraction;
        i.title = std::format("{} <-> {} MASKING {}-{}", upper(tracks[h.target].name), upper(tracks[h.masker].name), hzText(i.lowHz), hzText(i.highHz));
        i.detail = std::format("{} is within {:.0f} dB of (or louder than) {} in this range during {:.0f} % of the time {} is present",
                               tracks[h.masker].name, s.maskingMarginDb, tracks[h.target].name, h.fraction * 100.0, tracks[h.target].name);
        const double centre = std::sqrt(i.lowHz * i.highHz);
        const double bw = std::max(0.3, std::log2(i.highHz / i.lowHz));
        i.suggestions.push_back({std::format("DYNAMIC SPACE on {}: cut {} only while {} is active", tracks[h.masker].name, hzText(centre), tracks[h.target].name),
                                 "AddInsert",
                                 {{"channelId", tracks[h.masker].channelId}, {"typeId", "roy.dynamicspace"}, {"sidechain", tracks[h.target].channelId},
                                  {"params", {{"frequency", std::round(centre)}, {"bandwidth", bw}, {"maxReduction", -4.0}}}}});
        i.suggestions.push_back({std::format("Static EQ cut on {} at {} (-2.5 dB)", tracks[h.masker].name, hzText(centre)), "AddInsert",
                                 {{"channelId", tracks[h.masker].channelId}, {"typeId", "roy.eq"},
                                  {"params", {{"band2Freq", std::round(centre)}, {"band2Gain", -2.5}, {"band2Q", 1.4}}}}});
        i.suggestions.push_back({"Separate the two in the stereo field (pan)", "", {}});
        rep.issues.push_back(i);
    }

    // ---- low-end collisions and phase between tracks ----
    for (size_t a = 0; a < tracks.size(); ++a)
        for (size_t b = a + 1; b < tracks.size(); ++b) {
            const auto& ba = bands[a];
            const auto& bb = bands[b];
            const size_t frames = std::min(ba.size(), bb.size());
            if (!frames) continue;
            auto lowDb = [](const std::vector<float>& row) {
                double e = 0;
                for (size_t k = 0; k < 7; ++k) e += std::pow(10.0, row[k] / 10.0); // 25..100 Hz
                return 10.0 * std::log10(e + 1e-20);
            };
            double maxA = -200, maxB = -200;
            for (size_t f = 0; f < frames; ++f) {
                maxA = std::max(maxA, lowDb(ba[f]));
                maxB = std::max(maxB, lowDb(bb[f]));
            }
            if (maxA < -40 || maxB < -40) continue;
            int both = 0, anyA = 0;
            for (size_t f = 0; f < frames; ++f) {
                const bool aa = lowDb(ba[f]) > maxA - 10, bb2 = lowDb(bb[f]) > maxB - 10;
                anyA += aa;
                both += aa && bb2;
            }
            const double overlap = anyA ? static_cast<double>(both) / anyA : 0.0;
            // phase: low-band correlation while both are present
            const auto la = lowPassed(mono[a], sr, 150.0), lb = lowPassed(mono[b], sr, 150.0);
            const double corr = correlation(la, lb);
            if (corr < -0.3 && overlap > 0.2) {
                MixIssue i;
                i.type = "phase";
                i.severity = "problem";
                i.tracks = {tracks[a].name, tracks[b].name};
                i.value = corr;
                i.lowHz = 20;
                i.highHz = 150;
                i.title = std::format("{} <-> {} LOW-END PHASE CANCELLATION", upper(tracks[a].name), upper(tracks[b].name));
                i.detail = std::format("low-band correlation {:.2f} while both play", corr);
                i.suggestions.push_back({std::format("Invert the polarity of {}", tracks[b].name), "InvertPhase", {{"channelId", tracks[b].channelId}}});
                rep.issues.push_back(i);
            } else if (overlap > 0.3) {
                // the more sustained track (lower crest) gets ducked by the punchier one
                auto crest = [](const std::vector<float>& x) {
                    double p = 0, s2 = 0;
                    for (float v : x) {
                        p = std::max(p, static_cast<double>(std::fabs(v)));
                        s2 += static_cast<double>(v) * v;
                    }
                    return p / std::sqrt(s2 / std::max<size_t>(1, x.size()) + 1e-20);
                };
                const bool aPunchy = crest(la) > crest(lb);
                const size_t key = aPunchy ? a : b, duck = aPunchy ? b : a;
                MixIssue i;
                i.type = "low_end_collision";
                i.severity = overlap > 0.6 ? "warning" : "info";
                i.tracks = {tracks[a].name, tracks[b].name};
                i.value = overlap;
                i.lowHz = 20;
                i.highHz = 120;
                i.title = std::format("{} <-> {} LOW-END COLLISION", upper(tracks[a].name), upper(tracks[b].name));
                i.detail = std::format("both carry strong energy below 120 Hz at the same time ({:.0f} % overlap)", overlap * 100.0);
                i.suggestions.push_back({std::format("Sidechain-compress {} from {}", tracks[duck].name, tracks[key].name), "AddInsert",
                                         {{"channelId", tracks[duck].channelId}, {"typeId", "roy.compressor"}, {"sidechain", tracks[key].channelId},
                                          {"params", {{"sidechain", 1.0}, {"ratio", 4.0}, {"attack", 1.0}, {"release", 120.0}, {"threshold", -24.0}}}}});
                i.suggestions.push_back({std::format("DYNAMIC SPACE on {} around 60 Hz keyed by {}", tracks[duck].name, tracks[key].name), "AddInsert",
                                         {{"channelId", tracks[duck].channelId}, {"typeId", "roy.dynamicspace"}, {"sidechain", tracks[key].channelId},
                                          {"params", {{"frequency", 60.0}, {"bandwidth", 1.5}, {"maxReduction", -6.0}}}}});
                rep.issues.push_back(i);
            }
        }

    // ---- vocal sibilance and resonances ----
    for (size_t t = 0; t < tracks.size(); ++t) {
        if (!isVocal(tracks[t].role)) continue;
        const auto& x = mono[t];
        auto eAll = dsp::bandEnergy(x.data(), static_cast<int64_t>(x.size()), sr, 20, sr / 2, 2048, 1024);
        auto eSib = dsp::bandEnergy(x.data(), static_cast<int64_t>(x.size()), sr, 5000, std::min(10000.0, sr / 2), 2048, 1024);
        std::vector<float> ratios;
        float mxE = 0;
        for (float e : eAll) mxE = std::max(mxE, e);
        for (size_t i = 0; i < eAll.size(); ++i)
            if (eAll[i] > mxE * 1e-3f) ratios.push_back(static_cast<float>(10 * std::log10((eSib[i] + 1e-20) / (eAll[i] + 1e-20))));
        const double sib = ratios.empty() ? -120 : dsp::percentile(ratios, 0.95);
        if (sib > -8.0) {
            MixIssue i;
            i.type = "sibilance";
            i.severity = "warning";
            i.tracks = {tracks[t].name};
            i.value = sib;
            i.lowHz = 5000;
            i.highHz = 10000;
            i.title = std::format("{} SIBILANCE", upper(tracks[t].name));
            i.detail = std::format("5-10 kHz share reaches {:.1f} dB in sibilant moments", sib);
            i.suggestions.push_back({"De-esser around 6.5 kHz", "AddInsert",
                                     {{"channelId", tracks[t].channelId}, {"typeId", "roy.deesser"}, {"params", {{"frequency", 6500.0}}}}});
            rep.issues.push_back(i);
        }
    }

    // ---- master: headroom, stereo, phase ----
    if (!master.empty() && !master[0].empty()) {
        auto st = dsp::measureLoudness(master, sr);
        rep.masterPeakDb = st.samplePeakDb;
        rep.masterTruePeakDb = st.truePeakDb;
        rep.masterLufs = st.integratedLufs;
        const auto& L = master[0];
        const auto& R = master.size() > 1 ? master[1] : master[0];
        rep.masterCorrelation = correlation(L, R);
        std::vector<float> mid(L.size()), side(L.size());
        for (size_t i = 0; i < L.size(); ++i) {
            mid[i] = 0.5f * (L[i] + R[i]);
            side[i] = 0.5f * (L[i] - R[i]);
        }
        auto lm = lowPassed(mid, sr, 120.0), ls = lowPassed(side, sr, 120.0);
        double em = 0, es = 0;
        for (size_t i = 0; i < lm.size(); ++i) {
            em += static_cast<double>(lm[i]) * lm[i];
            es += static_cast<double>(ls[i]) * ls[i];
        }
        rep.masterLowSideRatioDb = 10 * std::log10((es + 1e-20) / (em + 1e-20));
        if (st.truePeakDb > -1.0) {
            MixIssue i;
            i.type = st.samplePeakDb > 0.0 ? "clipping" : "headroom";
            i.severity = st.samplePeakDb > 0.0 ? "problem" : "warning";
            i.tracks = {"MASTER"};
            i.value = st.truePeakDb;
            i.title = st.samplePeakDb > 0.0 ? "MASTER CLIPPING" : "MASTER HEADROOM";
            i.detail = std::format("sample peak {:.1f} dBFS, true peak {:.1f} dBTP (lossy encoding needs about -1 dBTP)", st.samplePeakDb, st.truePeakDb);
            i.suggestions.push_back({"True-peak limiter on the master (ceiling -1 dBTP)", "AddInsert",
                                     {{"master", true}, {"typeId", "roy.limiter"}, {"params", {{"ceiling", -1.0}, {"truePeak", 1.0}}}}});
            rep.issues.push_back(i);
        }
        if (rep.masterCorrelation < 0.2) {
            MixIssue i;
            i.type = "stereo";
            i.severity = rep.masterCorrelation < 0.0 ? "problem" : "warning";
            i.tracks = {"MASTER"};
            i.value = rep.masterCorrelation;
            i.title = "MASTER STEREO / PHASE";
            i.detail = std::format("L/R correlation {:.2f}: parts of the mix cancel in mono", rep.masterCorrelation);
            i.suggestions.push_back({"Check wide effects / polarity; reduce width", "", {}});
            rep.issues.push_back(i);
        }
        if (rep.masterLowSideRatioDb > -12.0 && em > 1e-6) {
            MixIssue i;
            i.type = "stereo";
            i.severity = "warning";
            i.tracks = {"MASTER"};
            i.value = rep.masterLowSideRatioDb;
            i.lowHz = 20;
            i.highHz = 120;
            i.title = "WIDE LOW END";
            i.detail = std::format("side energy below 120 Hz is {:.1f} dB relative to mid (club / vinyl / mono playback issue)", rep.masterLowSideRatioDb);
            i.suggestions.push_back({"RoY Stereo on the master: mono below 120 Hz", "AddInsert",
                                     {{"master", true}, {"typeId", "roy.stereo"}, {"params", {{"monoBass", 120.0}}}}});
            rep.issues.push_back(i);
        }
    }
    return rep;
}

bool renderForAnalysis(AudioEngine& engine, ProjectRuntime& runtime, const Project& project, double startBeat, double endBeat,
                       std::vector<TrackAudio>& tracks, Channels& master, std::string* error) {
    if (!runtime.rebuild(project)) {
        if (error) *error = "project could not be compiled";
        return false;
    }
    const double sr = engine.sampleRate();
    const int64_t s0 = static_cast<int64_t>(project.tempo.beatToSample(startBeat, sr));
    const int64_t s1 = static_cast<int64_t>(project.tempo.beatToSample(endBeat, sr));
    ChannelCapture cap;
    std::vector<std::string> ids;
    for (auto& t : project.tracks) ids.push_back(t.channelId);
    cap.allocate(ids, s1 - s0);
    OfflineRenderOptions o;
    o.startSample = s0;
    o.numFrames = s1 - s0;
    o.capture = &cap;
    master = renderOffline(engine, o, error);
    if (master.empty()) return false;
    tracks.clear();
    for (size_t k = 0; k < project.tracks.size(); ++k) {
        const auto& t = project.tracks[k];
        tracks.push_back({t.id, t.channelId, t.name, t.role, cap.data[k]});
    }
    return true;
}

} // namespace roy::mixi
