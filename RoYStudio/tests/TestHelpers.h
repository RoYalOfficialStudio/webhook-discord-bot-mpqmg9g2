#pragma once
// Shared helpers for tests. Everything here is explicitly TEST/MOCK code.
#include "audio/AudioEngine.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "core/Files.h"
#include "core/Math.h"
#include "project/Project.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace roytest {

inline std::shared_ptr<roy::AudioData> makeSine(double sr, double freq, double seconds, float amp = 0.5f, int channels = 2) {
    auto d = std::make_shared<roy::AudioData>();
    d->sampleRate = sr;
    d->numChannels = channels;
    d->numFrames = static_cast<int64_t>(seconds * sr);
    d->channels.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(d->numFrames)));
    for (int64_t i = 0; i < d->numFrames; ++i)
        for (int c = 0; c < channels; ++c)
            d->channels[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                amp * static_cast<float>(std::sin(roy::kTwoPi * freq * static_cast<double>(i) / sr));
    return d;
}

inline std::shared_ptr<roy::AudioData> makeImpulse(double sr, double seconds, int64_t at, float amp = 1.0f) {
    auto d = std::make_shared<roy::AudioData>();
    d->sampleRate = sr;
    d->numChannels = 2;
    d->numFrames = static_cast<int64_t>(seconds * sr);
    d->channels.assign(2, std::vector<float>(static_cast<size_t>(d->numFrames)));
    d->channels[0][static_cast<size_t>(at)] = amp;
    d->channels[1][static_cast<size_t>(at)] = amp;
    return d;
}

// Registers an in-memory asset with both the project and the runtime cache.
inline std::string addMemoryAsset(roy::Project& p, roy::ProjectRuntime& rt, std::shared_ptr<roy::AudioData> d,
                                  const std::string& name = "mem") {
    roy::AudioAsset a;
    a.id = roy::files::newId();
    a.path = "memory://" + name; // MOCK path: data is injected directly into the cache
    a.originalName = name;
    a.sampleRate = d->sampleRate;
    a.channels = d->numChannels;
    a.frames = d->numFrames;
    p.assets.push_back(a);
    d->assetId = a.id;
    rt.addLoadedAsset(a.id, d);
    return a.id;
}

inline roy::AudioClip& addClip(roy::Track& t, const std::string& assetId, double startBeat, double lengthBeats) {
    roy::AudioClip c;
    c.id = roy::files::newId();
    c.assetId = assetId;
    c.name = "clip";
    c.startBeat = startBeat;
    c.lengthBeats = lengthBeats;
    t.audioClips.push_back(c);
    return t.audioClips.back();
}

inline std::vector<std::vector<float>> render(roy::AudioEngine& e, int64_t frames, int block = 256, int64_t start = 0) {
    roy::OfflineRenderOptions o;
    o.numFrames = frames;
    o.blockSize = block;
    o.startSample = start;
    return roy::renderOffline(e, o);
}

inline float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float p = 0;
    for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}

inline double rms(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    double s = 0;
    size_t n = 0;
    for (size_t i = from; i < std::min(to, v.size()); ++i, ++n) s += static_cast<double>(v[i]) * v[i];
    return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

inline size_t argmaxAbs(const std::vector<float>& v) {
    size_t best = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
    return best;
}

inline bool allFinite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

} // namespace roytest
