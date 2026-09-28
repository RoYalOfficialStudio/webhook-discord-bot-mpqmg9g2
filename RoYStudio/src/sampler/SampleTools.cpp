#include "sampler/SampleTools.h"
#include "core/Math.h"
#include "vocal/PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace roy::sampler {

Channels trim(const Channels& in, int64_t s, int64_t e) {
    Channels out;
    for (auto& c : in) {
        const int64_t n = static_cast<int64_t>(c.size());
        const int64_t a = std::clamp<int64_t>(s, 0, n), b = std::clamp<int64_t>(e < 0 ? n : e, a, n);
        out.emplace_back(c.begin() + a, c.begin() + b);
    }
    return out;
}

Channels trimSilence(const Channels& in, double thr, int64_t* removedStart) {
    if (in.empty() || in[0].empty()) return in;
    const float g = static_cast<float>(dbToGain(thr));
    const int64_t n = static_cast<int64_t>(in[0].size());
    int64_t a = 0, b = n;
    auto loud = [&](int64_t i) {
        for (auto& c : in)
            if (std::fabs(c[static_cast<size_t>(i)]) > g) return true;
        return false;
    };
    while (a < n && !loud(a)) ++a;
    while (b > a && !loud(b - 1)) --b;
    if (removedStart) *removedStart = a;
    return trim(in, a, b);
}

Channels normalize(const Channels& in, double targetDb) {
    float pk = 0;
    for (auto& c : in)
        for (float v : c) pk = std::max(pk, std::fabs(v));
    if (pk <= 0) return in;
    const float g = static_cast<float>(dbToGain(targetDb)) / pk;
    Channels out = in;
    for (auto& c : out)
        for (auto& v : c) v *= g;
    return out;
}

Channels reverse(const Channels& in) {
    Channels out = in;
    for (auto& c : out) std::reverse(c.begin(), c.end());
    return out;
}

Channels fade(const Channels& in, int64_t fi, int64_t fo) {
    Channels out = in;
    for (auto& c : out) {
        const int64_t n = static_cast<int64_t>(c.size());
        for (int64_t i = 0; i < std::min(fi, n); ++i) c[static_cast<size_t>(i)] *= static_cast<float>(i) / static_cast<float>(fi);
        for (int64_t i = 0; i < std::min(fo, n); ++i) c[static_cast<size_t>(n - 1 - i)] *= static_cast<float>(i) / static_cast<float>(fo);
    }
    return out;
}

std::vector<int64_t> detectTransients(const Channels& in, double sr, double sensitivity) {
    const auto mono = dsp::mixToMono(in);
    dsp::OnsetSettings s;
    s.sensitivity = sensitivity;
    s.minGapSeconds = 0.04;
    std::vector<int64_t> out;
    for (auto& o : dsp::detectOnsets(mono.data(), static_cast<int64_t>(mono.size()), sr, s))
        out.push_back(static_cast<int64_t>(std::llround(o.time * sr)));
    return out;
}

std::vector<std::pair<int64_t, int64_t>> slicesFromTransients(const Channels& in, double sr, double sensitivity) {
    std::vector<std::pair<int64_t, int64_t>> out;
    if (in.empty()) return out;
    const int64_t n = static_cast<int64_t>(in[0].size());
    auto t = detectTransients(in, sr, sensitivity);
    if (t.empty() || t.front() > static_cast<int64_t>(0.01 * sr)) t.insert(t.begin(), 0);
    for (size_t i = 0; i < t.size(); ++i) out.push_back({t[i], i + 1 < t.size() ? t[i + 1] : n});
    return out;
}

std::vector<std::pair<int64_t, int64_t>> slicesGrid(int64_t length, int count) {
    std::vector<std::pair<int64_t, int64_t>> out;
    count = std::max(1, count);
    for (int i = 0; i < count; ++i) out.push_back({length * i / count, length * (i + 1) / count});
    return out;
}

std::vector<SamplerZone> sliceToPads(const std::string& assetId, const std::vector<std::pair<int64_t, int64_t>>& slices,
                                     int firstNote, bool choke) {
    std::vector<SamplerZone> zones;
    for (size_t i = 0; i < slices.size() && firstNote + static_cast<int>(i) <= 127; ++i) {
        SamplerZone z;
        z.assetId = assetId;
        z.rootNote = z.lowNote = z.highNote = firstNote + static_cast<int>(i);
        z.start = slices[i].first;
        z.end = slices[i].second;
        z.oneShot = true;
        z.chokeGroup = choke ? 1 : 0;
        zones.push_back(z);
    }
    return zones;
}

RootNote detectRootNote(const Channels& in, double sr) {
    RootNote r;
    vocal::PitchDetectorSettings s;
    s.minHz = 25.0;
    s.maxHz = 2000.0;
    s.windowSeconds = 0.08; // long window for low notes (808s)
    s.hop = 512;
    auto track = vocal::detectPitch(in, sr, s);
    std::vector<double> v;
    double conf = 0;
    // skip the attack: use frames after 50 ms
    for (auto& f : track.frames)
        if (f.voiced && f.time > 0.05) {
            v.push_back(f.midi);
            conf += f.confidence;
        }
    if (v.size() < 3) return r;
    std::sort(v.begin(), v.end());
    const double m = v[v.size() / 2];
    r.note = static_cast<int>(std::lround(m));
    r.cents = (m - r.note) * 100.0;
    r.confidence = conf / static_cast<double>(v.size());
    return r;
}

SampleInfo analyzeSample(const Channels& in, double sr) {
    SampleInfo i;
    if (in.empty() || in[0].empty()) return i;
    const auto mono = dsp::mixToMono(in);
    const int64_t n = static_cast<int64_t>(mono.size());
    i.durationSec = static_cast<double>(n) / sr;
    float pk = 0;
    for (float v : mono) pk = std::max(pk, std::fabs(v));
    i.peakDb = gainToDb(static_cast<double>(pk));
    i.transients = static_cast<int>(detectTransients(in, sr).size());
    if (i.durationSec >= 2.0) {
        auto t = dsp::estimateBpm(mono.data(), n, sr);
        i.bpm = t.bpm;
        i.bpmConfidence = t.confidence;
        i.looksLikeLoop = i.transients >= 4 && t.confidence > 0.2;
    }
    auto k = dsp::estimateKey(mono.data(), n, sr);
    i.keyRoot = k.root;
    i.keyMinor = k.minor;
    i.keyConfidence = k.confidence;
    i.root = detectRootNote(in, sr);
    return i;
}

} // namespace roy::sampler
