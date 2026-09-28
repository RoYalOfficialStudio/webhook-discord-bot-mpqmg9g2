#include "master/Mastering.h"
#include "audio/Processor.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "effects/Dynamics.h"
#include "intelligence/MixIntelligence.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::master {

const std::vector<MasterPreset>& presets() {
    static const std::vector<MasterPreset> p = {
        {"streaming", "Streaming (-14 LUFS, -1 dBTP)", -14.0, -1.0,
         {{"roy.eq", "Master EQ", {{"lowCut", 25.0}}},
          {"roy.compressor", "Glue", {{"threshold", -12.0}, {"ratio", 2.0}, {"attack", 30.0}, {"release", 200.0}, {"knee", 6.0}}},
          {"roy.saturation", "Warmth", {{"drive", 2.0}, {"mix", 0.5}, {"output", -1.0}}},
          {"roy.stereo", "Stereo", {{"monoBass", 100.0}}},
          {"roy.limiter", "Limiter", {{"ceiling", -1.0}, {"truePeak", 1.0}, {"release", 80.0}}},
          {"roy.analyzer", "Meter", {}}}},
        {"dynamic", "Dynamic (-16 LUFS, -1 dBTP)", -16.0, -1.0,
         {{"roy.eq", "Master EQ", {{"lowCut", 25.0}}},
          {"roy.compressor", "Glue", {{"threshold", -14.0}, {"ratio", 1.5}, {"attack", 30.0}, {"release", 250.0}, {"knee", 8.0}}},
          {"roy.limiter", "Limiter", {{"ceiling", -1.0}, {"truePeak", 1.0}, {"release", 120.0}}},
          {"roy.analyzer", "Meter", {}}}},
        {"club", "Club (-9 LUFS, -0.8 dBTP)", -9.0, -0.8,
         {{"roy.eq", "Master EQ", {{"lowCut", 28.0}}},
          {"roy.compressor", "Glue", {{"threshold", -10.0}, {"ratio", 3.0}, {"attack", 20.0}, {"release", 120.0}, {"knee", 6.0}}},
          {"roy.saturation", "Drive", {{"drive", 4.0}, {"mix", 0.6}, {"output", -2.0}}},
          {"roy.stereo", "Stereo", {{"monoBass", 120.0}}},
          {"roy.clipper", "Clipper", {{"ceiling", -0.5}, {"softness", 0.5}}},
          {"roy.limiter", "Limiter", {{"ceiling", -0.8}, {"truePeak", 1.0}, {"release", 50.0}}},
          {"roy.analyzer", "Meter", {}}}},
        {"voice", "Podcast / Voice (-16 LUFS, -1 dBTP)", -16.0, -1.0,
         {{"roy.eq", "Voice EQ", {{"lowCut", 80.0}}},
          {"roy.compressor", "Leveler", {{"threshold", -20.0}, {"ratio", 3.0}, {"attack", 10.0}, {"release", 150.0}}},
          {"roy.limiter", "Limiter", {{"ceiling", -1.0}, {"truePeak", 1.0}}},
          {"roy.analyzer", "Meter", {}}}},
    };
    return p;
}

const MasterPreset* findPreset(const std::string& id) {
    for (auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

Channels processChain(const Channels& in, double sr, const MasterPreset& preset, double limiterInputGainDb, double* maxGrOut) {
    registerBuiltinProcessors();
    Channels x = in;
    if (x.size() == 1) x.push_back(x[0]);
    const size_t n = x.empty() ? 0 : x[0].size();
    double maxGr = 0;
    for (auto& m : preset.chain) {
        auto p = ProcessorFactory::instance().create(m.typeId);
        if (!p) continue;
        for (auto& [k, v] : m.params.items())
            if (v.is_number()) p->setParam(k, v.get<float>());
        if (m.typeId == "roy.limiter") p->setParam("inputGain", static_cast<float>(limiterInputGainDb));
        p->prepare(sr, 1024);
        // latency compensation: process `lat` extra zeros, then drop the first `lat` samples
        const size_t lat = static_cast<size_t>(std::max(0, p->latencySamples()));
        for (auto& c : x) c.resize(n + lat, 0.0f);
        const size_t len = n + lat;
        for (size_t off = 0; off < len; off += 1024) {
            const int k = static_cast<int>(std::min<size_t>(1024, len - off));
            float* ptr[2] = {x[0].data() + off, x[1].data() + off};
            AudioBlock b{ptr, 2, k};
            p->process(b, nullptr, nullptr, 0);
            if (auto* lim = dynamic_cast<RoyLimiter*>(p.get())) maxGr = std::min(maxGr, static_cast<double>(lim->gainReductionDb()));
        }
        for (auto& c : x) c.erase(c.begin(), c.begin() + static_cast<long>(lat));
    }
    for (auto& c : x) c.resize(n);
    if (maxGrOut) *maxGrOut = maxGr;
    if (in.size() == 1) x.resize(1);
    return x;
}

AssistantResult assist(const Channels& pre, double sr, const MasterPreset& preset, double maxGr) {
    AssistantResult r;
    r.before = dsp::measureLoudness(pre, sr);
    if (r.before.integratedLufs <= -69.0) {
        r.notes.push_back("the mix is silent - nothing to master");
        return r;
    }
    // bisection over the limiter input gain (0..30 dB)
    double lo = 0.0, hi = 30.0, best = 0.0, bestGr = 0.0;
    dsp::LoudnessStats bestStats;
    bool found = false;
    for (int it = 0; it < 12; ++it) {
        const double mid = it == 0 ? 0.0 : 0.5 * (lo + hi);
        double gr = 0;
        auto out = processChain(pre, sr, preset, mid, &gr);
        auto st = dsp::measureLoudness(out, sr);
        if (it == 0) {
            best = 0;
            bestGr = gr;
            bestStats = st;
            if (st.integratedLufs >= preset.targetLufs) { // already loud enough
                found = true;
                break;
            }
            continue;
        }
        if (-gr > maxGr) { // too much limiting: stay below
            hi = mid;
            continue;
        }
        best = mid;
        bestGr = gr;
        bestStats = st;
        if (std::fabs(st.integratedLufs - preset.targetLufs) < 0.1) {
            found = true;
            break;
        }
        if (st.integratedLufs < preset.targetLufs) lo = mid;
        else hi = mid;
    }
    r.limiterInputGainDb = best;
    r.maxGainReductionDb = bestGr;
    r.after = bestStats;
    r.targetReached = found || std::fabs(bestStats.integratedLufs - preset.targetLufs) < 0.3;
    if (!r.targetReached)
        r.notes.push_back(std::format("target {:.1f} LUFS not reached within {:.0f} dB of limiting: reached {:.1f} LUFS. "
                                      "Consider mix/compression changes instead of more limiting.",
                                      preset.targetLufs, maxGr, bestStats.integratedLufs));
    if (-bestGr > 4.0) r.notes.push_back(std::format("limiter reduces peaks by up to {:.1f} dB - transients will be audibly softened", -bestGr));
    if (r.after.dynamicRangeDb < 7.0) r.notes.push_back(std::format("peak-to-loudness ratio {:.1f} dB: very dense master", r.after.dynamicRangeDb));
    if (r.after.truePeakDb > preset.ceilingDb + 0.3) r.notes.push_back("true peak above the ceiling - check the limiter settings");
    return r;
}

ReferenceComparison compareToReference(const Channels& mix, const Channels& ref, double sr) {
    ReferenceComparison c;
    c.mix = dsp::measureLoudness(mix, sr);
    c.reference = dsp::measureLoudness(ref, sr);
    auto corr = [](const Channels& a) {
        if (a.size() < 2) return 1.0;
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < a[0].size(); ++i) {
            ab += static_cast<double>(a[0][i]) * a[1][i];
            aa += static_cast<double>(a[0][i]) * a[0][i];
            bb += static_cast<double>(a[1][i]) * a[1][i];
        }
        return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 1.0;
    };
    c.correlationMix = corr(mix);
    c.correlationRef = corr(ref);
    const auto mm = dsp::mixToMono(mix), rm = dsp::mixToMono(ref);
    const auto sm = dsp::averageSpectrumDb(mm.data(), static_cast<int64_t>(mm.size()), sr, 8192);
    const auto sref = dsp::averageSpectrumDb(rm.data(), static_cast<int64_t>(rm.size()), sr, 8192);
    const double match = c.reference.integratedLufs - c.mix.integratedLufs; // loudness-matched comparison
    for (double centre : mixi::thirdOctaveCentres()) {
        const size_t b0 = static_cast<size_t>(centre / std::pow(2.0, 1.0 / 6.0) * 8192 / sr);
        const size_t b1 = std::max(b0 + 1, static_cast<size_t>(centre * std::pow(2.0, 1.0 / 6.0) * 8192 / sr));
        double em = 0, er = 0;
        for (size_t b = b0; b < b1 && b < sm.size(); ++b) {
            em += std::pow(10.0, sm[b] / 10.0);
            er += std::pow(10.0, sref[b] / 10.0);
        }
        c.bandDiffDb.push_back(10.0 * std::log10((em + 1e-20) / (er + 1e-20)) + match);
    }
    const double dl = c.mix.integratedLufs - c.reference.integratedLufs;
    c.notes.push_back(std::format("loudness: mix {:.1f} LUFS vs reference {:.1f} LUFS ({:+.1f} LU)", c.mix.integratedLufs,
                                  c.reference.integratedLufs, dl));
    c.notes.push_back(std::format("true peak: mix {:.1f} dBTP vs reference {:.1f} dBTP", c.mix.truePeakDb, c.reference.truePeakDb));
    c.notes.push_back(std::format("loudness range: mix {:.1f} LU vs reference {:.1f} LU", c.mix.loudnessRangeLu, c.reference.loudnessRangeLu));
    const auto& centres = mixi::thirdOctaveCentres();
    for (size_t k = 0; k < c.bandDiffDb.size(); ++k)
        if (std::fabs(c.bandDiffDb[k]) > 4.0 && centres[k] >= 40 && centres[k] <= 12500)
            c.notes.push_back(std::format("{:.0f} Hz band is {:+.1f} dB vs reference (loudness matched)", centres[k], c.bandDiffDb[k]));
    return c;
}

} // namespace roy::master
