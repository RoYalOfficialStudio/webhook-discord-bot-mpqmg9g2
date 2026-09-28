#include "audio/PreviewPlayer.h"

#include <algorithm>

namespace roy {

void PreviewPlayer::play(std::shared_ptr<const AudioData> data, float gain, bool loop, uint64_t engineBlock) {
    std::lock_guard<std::mutex> lk(keepMutex_);
    // the previous data stays alive for a few blocks (the audio thread may still read it)
    for (auto& k : keep_)
        if (k.retireAfter == UINT64_MAX) k.retireAfter = engineBlock + 2;
    keep_.push_back({data, UINT64_MAX});
    gain_.store(gain, std::memory_order_relaxed);
    loop_.store(loop, std::memory_order_relaxed);
    pos_.store(0, std::memory_order_relaxed);
    data_.store(data.get(), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    playing_.store(data != nullptr, std::memory_order_release);
}

void PreviewPlayer::stop() { playing_.store(false, std::memory_order_release); }

void PreviewPlayer::collect(uint64_t engineBlock) {
    std::lock_guard<std::mutex> lk(keepMutex_);
    keep_.erase(std::remove_if(keep_.begin(), keep_.end(), [&](const Keep& k) { return k.retireAfter != UINT64_MAX && engineBlock > k.retireAfter; }),
                keep_.end());
}

void PreviewPlayer::render(float* const* out, int numOutputs, int offset, int frames) noexcept {
    if (!playing_.load(std::memory_order_acquire) || numOutputs < 1) return;
    const AudioData* d = data_.load(std::memory_order_acquire);
    if (!d || d->numFrames <= 0) return;
    const uint32_t gen = generation_.load(std::memory_order_acquire);
    if (gen != seenGeneration_) {
        seenGeneration_ = gen;
        fadeIn_ = 0;
    }
    const float g = gain_.load(std::memory_order_relaxed);
    const bool loop = loop_.load(std::memory_order_relaxed);
    int64_t pos = pos_.load(std::memory_order_relaxed);
    const int rightCh = d->numChannels > 1 ? 1 : 0;
    for (int i = 0; i < frames; ++i) {
        if (pos >= d->numFrames) {
            if (!loop) {
                playing_.store(false, std::memory_order_release);
                break;
            }
            pos = 0;
        }
        const float f = fadeIn_ < 256 ? static_cast<float>(fadeIn_++) / 256.0f : 1.0f;
        const float l = d->channels[0][static_cast<size_t>(pos)] * g * f;
        const float r = d->channels[static_cast<size_t>(rightCh)][static_cast<size_t>(pos)] * g * f;
        out[0][offset + i] += l;
        if (numOutputs > 1) out[1][offset + i] += r;
        ++pos;
    }
    pos_.store(pos, std::memory_order_relaxed);
}

} // namespace roy
