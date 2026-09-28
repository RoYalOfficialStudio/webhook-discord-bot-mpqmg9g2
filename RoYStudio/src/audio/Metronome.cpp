#include "audio/Metronome.h"
#include "core/Math.h"

#include <cmath>

namespace roy {

namespace {
std::vector<float> makeClick(double sr, double freq, double durationSec) {
    const size_t n = static_cast<size_t>(durationSec * sr);
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        const double env = std::exp(-t * 60.0) * std::min(1.0, t * sr / 16.0); // tiny attack avoids a click-on-click
        v[i] = static_cast<float>(env * (0.7 * std::sin(kTwoPi * freq * t) + 0.3 * std::sin(kTwoPi * freq * 2.0 * t)));
    }
    return v;
}
} // namespace

void Metronome::prepare(double sampleRate) {
    sr_ = sampleRate;
    accent_ = makeClick(sr_, 1760.0, 0.06);
    normal_ = makeClick(sr_, 1320.0, 0.05);
    playing_ = -1;
    playPos_ = 0;
}

void Metronome::render(const TempoMap& tempo, int64_t timelineStart, int frames, float* left, float* right,
                       bool countIn) noexcept {
    const bool on = countIn || (enabled_.load(std::memory_order_relaxed) && !countInOnly_.load(std::memory_order_relaxed));
    const float gain = dbToGain(gainDb_.load(std::memory_order_relaxed));
    int cursor = 0;

    auto playUntil = [&](int end) {
        if (playing_ < 0) {
            cursor = end;
            return;
        }
        const auto& snd = playing_ == 0 ? accent_ : normal_;
        while (cursor < end && playPos_ < snd.size()) {
            const float s = snd[playPos_++] * gain;
            left[cursor] += s;
            right[cursor] += s;
            ++cursor;
        }
        if (playPos_ >= snd.size()) playing_ = -1;
        cursor = end;
    };

    if (on && accent_.size() > 0) {
        const double startBeat = tempo.sampleToBeat(static_cast<double>(timelineStart), sr_);
        const double endBeat = tempo.sampleToBeat(static_cast<double>(timelineStart + frames), sr_);
        // Click unit = signature denominator (quarter for x/4, eighth for x/8).
        const auto bb0 = tempo.beatToBarBeat(startBeat);
        const auto sig = tempo.signatureAtBar(bb0.bar);
        const double unit = 4.0 / sig.denominator;
        double k = std::ceil(startBeat / unit - 1e-9);
        for (int guard = 0; guard < 64; ++guard, k += 1.0) {
            const double beat = k * unit;
            if (beat >= endBeat) break;
            const int64_t at = static_cast<int64_t>(std::llround(tempo.beatToSample(beat, sr_)));
            int off = static_cast<int>(at - timelineStart);
            if (off < 0 || off >= frames) continue;
            playUntil(off);
            const auto bb = tempo.beatToBarBeat(beat + 1e-9);
            playing_ = bb.beatInBar < 0.5 ? 0 : 1;
            playPos_ = 0;
            clicks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    playUntil(frames);
}

} // namespace roy
