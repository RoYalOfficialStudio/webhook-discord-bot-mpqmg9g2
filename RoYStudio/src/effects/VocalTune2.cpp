#include "effects/VocalTune2.h"

#include "core/Math.h"
#include "midi/Scale.h"

#include <algorithm>
#include <cmath>

namespace roy {

RoyVocalTune2::RoyVocalTune2()
    : Processor({{"keyRoot", "Key Root", 0, 11, 0, "", 12},
                 {"keyScale", "Key Scale", 0, 12, 1, "", 13},
                 {"speed", "Retune Speed", 0, 400, 40, "ms"},
                 {"strength", "Strength", 0, 1, 1},
                 {"offKeyFilter", "Off-Key Filter", 0, 1, 1, "", 2},
                 {"allowChromatic", "Allow Chromatic", 0, 1, 0, "", 2},
                 {"threshold", "Threshold", 0, 50, 15, "cents"},
                 {"mix", "Dry/Wet", 0, 1, 1},
                 {"formant", "Formant Preserve", 0, 1, 1, "", 2},
                 {"formantShift", "Formant Shift", -6, 6, 0, "st"},
                 {"humanize", "Humanize", 0, 1, 0},
                 {"vibrato", "Keep Vibrato", 0, 1, 1},
                 {"confidence", "Min Confidence", 0, 1, 0.5f},
                 {"mode", "Mode (Custom/Natural/Modern/Hard)", 0, 3, 0, "", 4}}) {}

void RoyVocalTune2::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    tracker_.prepare(sr, 80.0, 1000.0);
    psola_.prepare(sr, 2, 80.0, tracker_.analysisDelaySamples());
    reset();
}

void RoyVocalTune2::reset() {
    tracker_.reset();
    psola_.reset();
    shift_ = targetShift_ = 0;
    center_ = 0;
    note_ = -1000;
    heldSec_ = 0;
    ratio_ = formant_ = 1;
    mixNow_ = p(Mix);
    sinceRatio_ = 0;
    wasVoiced_ = false;
}

void RoyVocalTune2::updateTarget() noexcept {
    const int mode = static_cast<int>(p(Mode) + 0.5f);
    double speed = p(Speed), humanize = p(Humanize), vibrato = p(Vibrato);
    if (mode == Natural) { speed = 60; humanize = 0.6; vibrato = 1.0; }
    else if (mode == Modern) { speed = 15; humanize = 0.3; vibrato = 0.7; }
    else if (mode == Hard) { speed = 0; humanize = 0; vibrato = 0; }
    const double hop = tracker_.hopSeconds();
    const bool ok = tracker_.voiced() && tracker_.confidence() >= p(Confidence);
    confOut_.store(tracker_.confidence(), std::memory_order_relaxed);
    if (!ok) { // unvoiced / uncertain: glide back to no correction, never jump
        targetShift_ = 0;
        effSpeedMs_ = std::max(speed, 60.0);
        heldSec_ = 0;
        wasVoiced_ = false;
        detected_.store(0.0, std::memory_order_relaxed);
        return;
    }
    const double m = tracker_.midi();
    detected_.store(m, std::memory_order_relaxed);
    if (!wasVoiced_) {
        center_ = m;
        note_ = -1000;
        sideRunSec_ = 0;
    }
    wasVoiced_ = true;
    // pitch centre: slow (150 ms) for vibrato/wobble, fast (20 ms) when the singer really moves to
    // another note (> 0.9 semitones away) - note changes are followed quickly, vibrato is not
    // A deviation that keeps its sign for > 130 ms is not vibrato (which swings around the centre) but
    // a pitch offset: then the centre follows fast as well.
    const int sign = m > center_ ? 1 : -1;
    sideRunSec_ = sign == lastSide_ ? sideRunSec_ + hop : 0.0;
    lastSide_ = sign;
    const double tau = (std::fabs(m - center_) > 0.9 || sideRunSec_ > 0.13) ? 0.02 : 0.15;
    center_ += (m - center_) * (1.0 - std::exp(-hop / tau));
    const double sung = vibrato * center_ + (1.0 - vibrato) * m; // the pitch that is moved to the note
    // target note from the centre, with hysteresis
    const Key key{static_cast<int>(p(KeyRoot)), scaleTypeByIndex(static_cast<int>(p(KeyScale)))};
    auto choose = [&](double pitch) {
        const int semi = static_cast<int>(std::lround(pitch));
        double t = semi;
        if (!key.contains(semi) && p(OffKeyFilter) > 0.5f && p(AllowChromatic) < 0.5f) t = key.nearestPitch(pitch);
        return t;
    };
    const double cand = choose(center_);
    if (note_ < -100 || (cand != note_ && std::fabs(center_ - note_) > 0.65)) {
        note_ = cand;
        heldSec_ = 0;
    } else {
        heldSec_ += hop;
    }
    double dev = note_ - sung;
    if (std::fabs(center_ - note_) * 100.0 <= p(Threshold)) dev = 0; // within threshold: natural
    double strength = p(Strength);
    effSpeedMs_ = speed;
    if (heldSec_ > 0.25 && humanize > 0) { // held note: gentler and slower
        strength *= 1.0 - 0.6 * humanize;
        effSpeedMs_ = speed * (1.0 + 3.0 * humanize) + 20.0 * humanize;
    }
    targetShift_ = std::clamp(dev * strength, -6.0, 6.0);
}

void RoyVocalTune2::process(const AudioBlock& io, const AudioBlock*, const NoteEvent*, int) noexcept {
    float* L = io.channel(0);
    float* R = io.numChannels > 1 ? io.channel(1) : io.channel(0);
    const double sr = sampleRate_;
    const float mixTarget = p(Mix);
    const float mixStep = 1.0f / static_cast<float>(0.02 * sr); // full sweep in 20 ms
    const bool formantOn = p(Formant) > 0.5f;
    const double fShift = std::pow(2.0, p(FormantShift) / 12.0);
    for (int i = 0; i < io.numFrames; ++i) {
        if (tracker_.push(0.5f * (L[i] + R[i]))) updateTarget();
        const double coef = effSpeedMs_ <= 0 ? 0.0 : std::exp(-1.0 / (effSpeedMs_ * 0.001 * sr));
        shift_ = targetShift_ + (shift_ - targetShift_) * coef;
        if (++sinceRatio_ >= 16) { // exp2 every 16 samples (counter runs across blocks: block-size independent)
            sinceRatio_ = 0;
            ratio_ = std::exp2(shift_ / 12.0);
            formant_ = formantOn ? fShift : ratio_ * fShift; // formant off: colour moves with the pitch
        }
        const bool voiced = tracker_.voiced() && tracker_.confidence() >= p(Confidence);
        const double period = voiced && tracker_.hz() > 0 ? sr / tracker_.hz() : 0.0;
        const float in[2] = {L[i], R[i]};
        float wet[2], dry[2];
        psola_.process(in, wet, dry, period, voiced ? ratio_ : 1.0, voiced ? formant_ : fShift);
        if (mixNow_ < mixTarget) mixNow_ = std::min(mixTarget, mixNow_ + mixStep);
        else if (mixNow_ > mixTarget) mixNow_ = std::max(mixTarget, mixNow_ - mixStep);
        L[i] = dry[0] + (wet[0] - dry[0]) * mixNow_;
        if (io.numChannels > 1) R[i] = dry[1] + (wet[1] - dry[1]) * mixNow_;
    }
    shiftOut_.store(shift_, std::memory_order_relaxed);
}

} // namespace roy
