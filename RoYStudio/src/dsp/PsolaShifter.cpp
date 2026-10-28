#include "dsp/PsolaShifter.h"

#include <algorithm>
#include <cmath>

namespace roy::dsp {

void PsolaShifter::prepare(double sampleRate, int channels, double minHz, int analysisDelay) {
    sr_ = sampleRate;
    ch_ = std::clamp(channels, 1, 2);
    Pmax_ = static_cast<int>(std::ceil(sr_ / minHz));
    Pdef_ = static_cast<int>(std::lround(sr_ / 190.0));
    D_ = std::max(0, analysisDelay);
    L_ = std::max(D_, Pmax_) + Pmax_ + 64;
    int64_t size = 1;
    while (size < L_ + 6 * Pmax_ + 2048) size <<= 1;
    mask_ = size - 1;
    for (int c = 0; c < 2; ++c) {
        in_[c].assign(static_cast<size_t>(size), 0.0f);
        oa_[c].assign(static_cast<size_t>(size), 0.0f);
    }
    ws_.assign(static_cast<size_t>(size), 0.0f);
    parPeriod_.assign(static_cast<size_t>(size), 0.0f);
    parRatio_.assign(static_cast<size_t>(size), 1.0f);
    parFormant_.assign(static_cast<size_t>(size), 1.0f);
    reset();
}

void PsolaShifter::reset() {
    for (int c = 0; c < 2; ++c) {
        std::fill(in_[c].begin(), in_[c].end(), 0.0f);
        std::fill(oa_[c].begin(), oa_[c].end(), 0.0f);
    }
    std::fill(ws_.begin(), ws_.end(), 0.0f);
    std::fill(parPeriod_.begin(), parPeriod_.end(), 0.0f);
    std::fill(parRatio_.begin(), parRatio_.end(), 1.0f);
    std::fill(parFormant_.begin(), parFormant_.end(), 1.0f);
    n_ = 0;
    nextMark_ = 0;
    nextSyn_ = -1;
    markHead_ = markCount_ = markCur_ = 0;
}

float PsolaShifter::read(const std::vector<float>& v, double pos) const noexcept {
    const double fl = std::floor(pos);
    const auto i = static_cast<int64_t>(fl);
    const float f = static_cast<float>(pos - fl);
    return v[static_cast<size_t>(i & mask_)] * (1 - f) + v[static_cast<size_t>((i + 1) & mask_)] * f;
}

void PsolaShifter::addGrain(double centre, const Mark& src, double formant) noexcept {
    const double H = src.period;
    const double inv = 1.0 / H;
    // with formant > 1 the grain is read faster: start earlier so it never reads past src.pos + H
    const double base = src.pos - (formant > 1.0 ? H * (formant - 1.0) : 0.0);
    const auto o0 = static_cast<int64_t>(std::floor(centre - H)) + 1, o1 = static_cast<int64_t>(std::ceil(centre + H)) - 1;
    for (int64_t o = o0; o <= o1; ++o) {
        const double k = static_cast<double>(o) - centre;
        const float w = static_cast<float>(0.5 + 0.5 * std::cos(3.141592653589793 * k * inv));
        if (w <= 0.0f) continue;
        const double rp = base + k * formant;
        for (int c = 0; c < ch_; ++c) at(oa_[c], o) += w * read(in_[c], rp);
        at(ws_, o) += w;
    }
}

void PsolaShifter::process(const float* in, float* wet, float* dry, double period, double ratio, double formantFactor) noexcept {
    for (int c = 0; c < ch_; ++c) at(in_[c], n_) = in[c];
    // parameters describe time n - D
    at(parPeriod_, n_ - D_) = static_cast<float>(period);
    at(parRatio_, n_ - D_) = static_cast<float>(std::clamp(ratio, kMinRatio, kMaxRatio));
    at(parFormant_, n_ - D_) = static_cast<float>(std::clamp(formantFactor, kMinRatio, kMaxRatio));
    const double now = static_cast<double>(n_);
    const double known = now - D_; // parameters are known up to here

    // analysis marks exactly one period apart (the period at the mark)
    if (nextMark_ + 1.0 <= known) {
        const double per = at(parPeriod_, static_cast<int64_t>(nextMark_));
        const double P = per >= 16.0 ? std::clamp(per, 16.0, static_cast<double>(Pmax_)) : static_cast<double>(Pdef_);
        marks_[markHead_] = {nextMark_, P};
        markHead_ = (markHead_ + 1) % 128;
        if (markCount_ < 128) ++markCount_;
        else markCur_ = std::max(0, markCur_ - 1);
        if (nextSyn_ < 0) nextSyn_ = nextMark_;
        nextMark_ += P;
    }

    // synthesis: grains whose parameters and source audio are complete
    for (int guard = 0; guard < 8 && nextSyn_ >= 0 && markCount_ > 0; ++guard) {
        auto markAt = [&](int i) -> const Mark& { return marks_[(markHead_ - markCount_ + i + 128 * 2) % 128]; };
        while (markCur_ + 1 < markCount_ && std::fabs(markAt(markCur_ + 1).pos - nextSyn_) <= std::fabs(markAt(markCur_).pos - nextSyn_))
            ++markCur_;
        const Mark& src = markAt(markCur_);
        if (nextSyn_ > known || src.pos + src.period + 2 > now || nextSyn_ + src.period + 1 > now) break; // not yet
        if (nextSyn_ - src.period <= now - L_) { // too late (should not happen): resynchronise
            nextSyn_ = now - L_ + src.period + 1;
            continue;
        }
        const auto si = static_cast<int64_t>(nextSyn_);
        const double ratio = at(parRatio_, si), formant = at(parFormant_, si);
        const bool identity = std::fabs(ratio - 1.0) < 1e-4 && std::fabs(formant - 1.0) < 1e-4;
        const double centre = identity ? src.pos : nextSyn_;
        addGrain(centre, src, formant);
        if (identity) nextSyn_ = markCur_ + 1 < markCount_ ? markAt(markCur_ + 1).pos : src.pos + src.period;
        else nextSyn_ = centre + std::max(8.0, src.period / ratio);
    }

    // output (fixed latency)
    const int64_t o = n_ - L_;
    const float w = at(ws_, o);
    for (int c = 0; c < ch_; ++c) {
        const float d = at(in_[c], o);
        dry[c] = d;
        wet[c] = w > 1e-3f ? at(oa_[c], o) / w : d;
        at(oa_[c], o) = 0.0f;
    }
    at(ws_, o) = 0.0f;
    ++n_;
}

} // namespace roy::dsp
