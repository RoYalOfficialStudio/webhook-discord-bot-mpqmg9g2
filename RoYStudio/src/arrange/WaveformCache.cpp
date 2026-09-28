#include "arrange/WaveformCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace roy {

namespace fs = std::filesystem;

void WaveformCache::build(const AudioData& d) {
    numChannels_ = d.numChannels;
    numFrames_ = d.numFrames;
    sampleRate_ = d.sampleRate;
    levels_.clear();
    int spp = kBaseSamplesPerPeak;
    // level 0 from samples
    Level l0;
    l0.samplesPerPeak = spp;
    const size_t n0 = static_cast<size_t>((d.numFrames + spp - 1) / spp);
    l0.mins.assign(static_cast<size_t>(numChannels_), std::vector<float>(n0, 0.0f));
    l0.maxs.assign(static_cast<size_t>(numChannels_), std::vector<float>(n0, 0.0f));
    for (int c = 0; c < numChannels_; ++c) {
        const auto& src = d.channels[static_cast<size_t>(c)];
        for (size_t p = 0; p < n0; ++p) {
            const size_t s = p * static_cast<size_t>(spp);
            const size_t e = std::min(src.size(), s + static_cast<size_t>(spp));
            float mn = 0.0f, mx = 0.0f;
            if (s < e) {
                mn = mx = src[s];
                for (size_t i = s + 1; i < e; ++i) {
                    mn = std::min(mn, src[i]);
                    mx = std::max(mx, src[i]);
                }
            }
            l0.mins[static_cast<size_t>(c)][p] = mn;
            l0.maxs[static_cast<size_t>(c)][p] = mx;
        }
    }
    levels_.push_back(std::move(l0));
    for (int L = 1; L < kNumLevels; ++L) {
        const Level& prev = levels_.back();
        Level nl;
        nl.samplesPerPeak = prev.samplesPerPeak * kLevelFactor;
        const size_t n = (prev.mins.empty() ? 0 : prev.mins[0].size() + kLevelFactor - 1) / kLevelFactor;
        nl.mins.assign(static_cast<size_t>(numChannels_), std::vector<float>(n));
        nl.maxs.assign(static_cast<size_t>(numChannels_), std::vector<float>(n));
        for (int c = 0; c < numChannels_; ++c) {
            const auto& pm = prev.mins[static_cast<size_t>(c)];
            const auto& px = prev.maxs[static_cast<size_t>(c)];
            for (size_t p = 0; p < n; ++p) {
                float mn = 1e9f, mx = -1e9f;
                for (size_t k = p * kLevelFactor; k < std::min(pm.size(), (p + 1) * kLevelFactor); ++k) {
                    mn = std::min(mn, pm[k]);
                    mx = std::max(mx, px[k]);
                }
                nl.mins[static_cast<size_t>(c)][p] = mn > mx ? 0.0f : mn;
                nl.maxs[static_cast<size_t>(c)][p] = mn > mx ? 0.0f : mx;
            }
        }
        levels_.push_back(std::move(nl));
    }
}

void WaveformCache::getPeaks(int ch, double startSample, double endSample, int pixels, float* mins, float* maxs) const {
    if (pixels <= 0) return;
    std::fill(mins, mins + pixels, 0.0f);
    std::fill(maxs, maxs + pixels, 0.0f);
    if (levels_.empty() || ch < 0 || ch >= numChannels_ || endSample <= startSample) return;
    const double samplesPerPixel = (endSample - startSample) / pixels;
    // Finest level whose peak size is <= samplesPerPixel (at least level 0).
    size_t li = 0;
    for (size_t i = 0; i < levels_.size(); ++i)
        if (levels_[i].samplesPerPeak <= samplesPerPixel) li = i;
    const Level& L = levels_[li];
    const auto& mn = L.mins[static_cast<size_t>(ch)];
    const auto& mx = L.maxs[static_cast<size_t>(ch)];
    const double spp = L.samplesPerPeak;
    for (int x = 0; x < pixels; ++x) {
        const double s = startSample + x * samplesPerPixel;
        const double e = s + samplesPerPixel;
        int64_t a = static_cast<int64_t>(std::floor(s / spp));
        int64_t b = static_cast<int64_t>(std::ceil(e / spp));
        a = std::max<int64_t>(0, a);
        b = std::min<int64_t>(static_cast<int64_t>(mn.size()), std::max(b, a + 1));
        if (a >= static_cast<int64_t>(mn.size())) continue;
        float lo = mn[static_cast<size_t>(a)], hi = mx[static_cast<size_t>(a)];
        for (int64_t k = a + 1; k < b; ++k) {
            lo = std::min(lo, mn[static_cast<size_t>(k)]);
            hi = std::max(hi, mx[static_cast<size_t>(k)]);
        }
        mins[x] = lo;
        maxs[x] = hi;
    }
}

size_t WaveformCache::memoryBytes() const {
    size_t n = 0;
    for (auto& l : levels_)
        for (size_t c = 0; c < l.mins.size(); ++c) n += (l.mins[c].size() + l.maxs[c].size()) * sizeof(float);
    return n;
}

bool WaveformCache::save(const fs::path& file) const {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    std::ofstream o(tmp, std::ios::binary);
    if (!o) return false;
    const char magic[8] = {'R', 'O', 'Y', 'P', 'E', 'A', 'K', '1'};
    o.write(magic, 8);
    auto w64 = [&](int64_t v) { o.write(reinterpret_cast<const char*>(&v), 8); };
    auto wd = [&](double v) { o.write(reinterpret_cast<const char*>(&v), 8); };
    w64(numChannels_);
    w64(numFrames_);
    wd(sampleRate_);
    w64(static_cast<int64_t>(levels_.size()));
    for (auto& l : levels_) {
        w64(l.samplesPerPeak);
        w64(l.mins.empty() ? 0 : static_cast<int64_t>(l.mins[0].size()));
        for (int c = 0; c < numChannels_; ++c) {
            o.write(reinterpret_cast<const char*>(l.mins[static_cast<size_t>(c)].data()), static_cast<std::streamsize>(l.mins[static_cast<size_t>(c)].size() * 4));
            o.write(reinterpret_cast<const char*>(l.maxs[static_cast<size_t>(c)].data()), static_cast<std::streamsize>(l.maxs[static_cast<size_t>(c)].size() * 4));
        }
    }
    o.close();
    if (!o) return false;
    fs::rename(tmp, file, ec);
    return !ec;
}

bool WaveformCache::load(const fs::path& file, int64_t expectFrames, double expectRate) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    char magic[8];
    in.read(magic, 8);
    if (!in || std::memcmp(magic, "ROYPEAK1", 8) != 0) return false;
    auto r64 = [&]() { int64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; };
    auto rd = [&]() { double v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v; };
    const int64_t ch = r64(), frames = r64();
    const double rate = rd();
    if (frames != expectFrames || std::fabs(rate - expectRate) > 0.5 || ch <= 0 || ch > 64) return false;
    const int64_t nl = r64();
    if (nl <= 0 || nl > 16) return false;
    std::vector<Level> levels;
    for (int64_t i = 0; i < nl; ++i) {
        Level l;
        l.samplesPerPeak = static_cast<int>(r64());
        const int64_t n = r64();
        if (!in || n < 0 || n > (int64_t(1) << 32)) return false;
        l.mins.assign(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(n)));
        l.maxs.assign(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(n)));
        for (int64_t c = 0; c < ch; ++c) {
            in.read(reinterpret_cast<char*>(l.mins[static_cast<size_t>(c)].data()), n * 4);
            in.read(reinterpret_cast<char*>(l.maxs[static_cast<size_t>(c)].data()), n * 4);
        }
        if (!in) return false;
        levels.push_back(std::move(l));
    }
    numChannels_ = static_cast<int>(ch);
    numFrames_ = frames;
    sampleRate_ = rate;
    levels_ = std::move(levels);
    return true;
}

std::shared_ptr<const WaveformCache> WaveformStore::get(const std::string& assetId, const AudioData& data) {
    auto it = caches_.find(assetId);
    if (it != caches_.end() && it->second->numFrames() == data.numFrames) return it->second;
    auto c = std::make_shared<WaveformCache>();
    const fs::path file = dir_.empty() ? fs::path() : dir_ / (assetId + ".roypk");
    if (!file.empty() && c->load(file, data.numFrames, data.sampleRate)) {
        ++loaded_;
    } else {
        c->build(data);
        ++built_;
        if (!file.empty()) c->save(file);
    }
    caches_[assetId] = c;
    return c;
}

void WaveformStore::invalidate(const std::string& assetId) {
    caches_.erase(assetId);
    if (!dir_.empty()) {
        std::error_code ec;
        fs::remove(dir_ / (assetId + ".roypk"), ec);
    }
}

} // namespace roy
