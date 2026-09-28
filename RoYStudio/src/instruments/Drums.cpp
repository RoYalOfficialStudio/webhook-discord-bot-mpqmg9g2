#include "instruments/Drums.h"

#include <algorithm>
#include <algorithm>
#include <cmath>

namespace roy {

RoyDrums::RoyDrums()
    : Instrument({{"volume", "Volume", -60, 12, -3, "dB"},
                  {"kickTune", "Kick Tune", -12, 12, 0, "st"},
                  {"kickDecay", "Kick Decay", 0.05f, 2.0f, 0.35f, "s"},
                  {"kickClick", "Kick Click", 0, 1, 0.5f},
                  {"snareTone", "Snare Tone", 0, 1, 0.5f},
                  {"snareDecay", "Snare Decay", 0.05f, 1.0f, 0.18f, "s"},
                  {"clapDecay", "Clap Decay", 0.05f, 1.0f, 0.2f, "s"},
                  {"hatDecay", "Hat Decay", 0.01f, 0.5f, 0.05f, "s"},
                  {"openHatDecay", "Open Hat Decay", 0.05f, 2.0f, 0.4f, "s"},
                  {"percTune", "Perc Tune", -12, 12, 0, "st"},
                  {"width", "Stereo Width", 0, 1, 1}}) {
    for (auto& s : samples_) s.store(nullptr);
}

void RoyDrums::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoyDrums::reset() {
    for (auto& v : voices_) v = Voice{};
}

void RoyDrums::setSample(int note, std::shared_ptr<const AudioData> sample) {
    if (note < 0 || note > 127) return;
    std::lock_guard l(keepMutex_);
    // kept alive until the instrument is destroyed (the audio thread may still read it)
    if (sample && std::find(keepAlive_.begin(), keepAlive_.end(), sample) == keepAlive_.end()) keepAlive_.push_back(sample);
    samples_[static_cast<size_t>(note)].store(sample.get(), std::memory_order_release);
}

int RoyDrums::activeVoices() const {
    int n = 0;
    for (auto& v : voices_) n += v.kind != Kind::None ? 1 : 0;
    return n;
}

float RoyDrums::noise(Voice& v) noexcept {
    v.noise ^= v.noise << 13;
    v.noise ^= v.noise >> 17;
    v.noise ^= v.noise << 5;
    return static_cast<float>(static_cast<int32_t>(v.noise)) / 2147483648.0f;
}

void RoyDrums::handleEvent(const NoteEvent& e) noexcept {
    if (e.type == NoteEvent::AllNotesOff || e.type != NoteEvent::NoteOn || e.velocity <= 0) return; // one-shots ignore note-off
    Kind k = Kind::Perc;
    switch (e.note) {
    case 35: k = Kind::Sub808; break;
    case 36: k = Kind::Kick; break;
    case 37: k = Kind::Rim; break;
    case 38: case 40: k = Kind::Snare; break;
    case 39: k = Kind::Clap; break;
    case 42: case 44: k = Kind::ClosedHat; break;
    case 46: k = Kind::OpenHat; break;
    case 49: case 57: k = Kind::Fx; break;
    default: k = Kind::Perc; break;
    }
    const AudioData* smp = samples_[static_cast<size_t>(std::clamp<int>(e.note, 0, 127))].load(std::memory_order_acquire);
    if (smp) k = Kind::Sample;
    if (k == Kind::ClosedHat) // choke open hats
        for (auto& v : voices_)
            if (v.kind == Kind::OpenHat) v.kind = Kind::None;
    Voice* slot = nullptr;
    for (auto& v : voices_)
        if (v.kind == Kind::None) { slot = &v; break; }
    if (!slot) {
        slot = &voices_[0];
        for (auto& v : voices_)
            if (v.age < slot->age) slot = &v;
    }
    Voice& v = *slot;
    v = Voice{};
    v.kind = k;
    v.note = e.note;
    v.age = ++counter_;
    v.vel = std::clamp(e.velocity, 0.0f, 1.0f);
    const float width = p(Width);
    panGains(std::clamp(e.pan * width, -1.0f, 1.0f), v.panL, v.panR);
    v.pitchRatio = std::pow(2.0, e.detune / 12.0);
    v.noise = static_cast<uint32_t>(v.age * 2654435761u) | 1u;
    v.sample = smp;
    const double sr = sampleRate_;
    switch (k) {
    case Kind::Snare: v.f1.set(dsp::Biquad::Type::HighPass, sr, 1200.0, 0.7); break;
    case Kind::Clap: v.f1.set(dsp::Biquad::Type::BandPass, sr, 1300.0, 1.2); break;
    case Kind::ClosedHat:
    case Kind::OpenHat: v.f1.set(dsp::Biquad::Type::HighPass, sr, 7000.0 * v.pitchRatio, 0.8); break;
    case Kind::Rim: v.f1.set(dsp::Biquad::Type::BandPass, sr, 1700.0 * v.pitchRatio, 3.0); break;
    case Kind::Fx: v.f1.set(dsp::Biquad::Type::HighPass, sr, 2500.0, 0.6); break;
    default: break;
    }
}

float RoyDrums::renderVoice(Voice& v, double dt) noexcept {
    const double t = v.t;
    v.t += dt;
    float out = 0.0f;
    auto done = [&] { v.kind = Kind::None; };
    switch (v.kind) {
    case Kind::None: return 0.0f;
    case Kind::Kick: {
        const double tune = std::pow(2.0, p(KickTune) / 12.0) * v.pitchRatio;
        const double f = (48.0 + 110.0 * std::exp(-t / 0.035)) * tune;
        v.phase += f * dt;
        const double decay = p(KickDecay);
        const double amp = std::exp(-t / (decay * 0.4));
        const double click = p(KickClick) * std::exp(-t / 0.0025) * noise(v) * 0.6;
        out = static_cast<float>(std::sin(kTwoPi * v.phase) * amp + click);
        if (t > decay * 3.0) done();
        break;
    }
    case Kind::Sub808: {
        const double f = 49.0 * v.pitchRatio * (1.0 + 0.5 * std::exp(-t / 0.02));
        v.phase += f * dt;
        out = static_cast<float>(std::sin(kTwoPi * v.phase) * std::exp(-t / 0.5));
        if (t > 2.5) done();
        break;
    }
    case Kind::Snare: {
        const double tone = p(SnareTone), decay = p(SnareDecay);
        v.phase += 185.0 * v.pitchRatio * dt;
        v.phase2 += 330.0 * v.pitchRatio * dt;
        const double body = (std::sin(kTwoPi * v.phase) + 0.6 * std::sin(kTwoPi * v.phase2)) * std::exp(-t / 0.06) * tone;
        const double nz = v.f1.process(noise(v)) * std::exp(-t / (decay * 0.5)) * (1.3 - 0.5 * tone);
        out = static_cast<float>(0.6 * body + nz);
        if (t > decay * 4.0) done();
        break;
    }
    case Kind::Clap: {
        const double decay = p(ClapDecay);
        double env = 0;
        for (int b = 0; b < 3; ++b) {
            const double tb = t - b * 0.011;
            if (tb >= 0 && tb < 0.011) env = std::max(env, std::exp(-tb / 0.003));
        }
        if (t >= 0.022) env = std::max(env, 0.7 * std::exp(-(t - 0.022) / (decay * 0.5)));
        out = static_cast<float>(v.f1.process(noise(v)) * env * 2.0);
        if (t > decay * 4.0 + 0.03) done();
        break;
    }
    case Kind::ClosedHat:
    case Kind::OpenHat: {
        const double decay = v.kind == Kind::ClosedHat ? p(HatDecay) : p(OpenHatDecay);
        // metallic: inharmonic square partials + noise
        static const double ratios[6] = {2.0, 3.0, 4.16, 5.43, 6.79, 8.21};
        double metal = 0;
        v.phase += 40.0 * v.pitchRatio * dt;
        for (double r : ratios) metal += std::sin(kTwoPi * v.phase * r * 10.0) > 0 ? 1.0 : -1.0;
        const double x = 0.5 * metal / 6.0 + 0.7 * noise(v);
        out = static_cast<float>(v.f1.process(static_cast<float>(x)) * std::exp(-t / (decay * 0.4)));
        if (t > decay * 3.0) done();
        break;
    }
    case Kind::Perc: {
        const double tune = std::pow(2.0, p(PercTune) / 12.0) * v.pitchRatio * std::pow(2.0, (v.note - 47) / 12.0);
        const double f = (180.0 + 120.0 * std::exp(-t / 0.02)) * tune;
        v.phase += f * dt;
        out = static_cast<float>(std::sin(kTwoPi * v.phase) * std::exp(-t / 0.08));
        if (t > 0.5) done();
        break;
    }
    case Kind::Rim: {
        v.phase += 800.0 * v.pitchRatio * dt;
        out = static_cast<float>((0.7 * std::sin(kTwoPi * v.phase) + v.f1.process(noise(v))) * std::exp(-t / 0.012));
        if (t > 0.08) done();
        break;
    }
    case Kind::Fx: {
        out = static_cast<float>(v.f1.process(noise(v)) * std::exp(-t / 0.6) * std::min(1.0, t / 0.005));
        if (t > 3.0) done();
        break;
    }
    case Kind::Sample: {
        const AudioData* s = v.sample;
        if (!s || s->numFrames <= 0) {
            done();
            break;
        }
        const double pos = v.samplePos;
        const int64_t i = static_cast<int64_t>(pos);
        if (i + 1 >= s->numFrames) {
            done();
            break;
        }
        const float f = static_cast<float>(pos - static_cast<double>(i));
        out = s->sample(0, i) * (1 - f) + s->sample(0, i + 1) * f;
        v.samplePos += v.pitchRatio * s->sampleRate * dt;
        break;
    }
    }
    return out * v.vel;
}

void RoyDrums::render(const AudioBlock& io, int start, int end) noexcept {
    const double dt = 1.0 / sampleRate_;
    const float vol = dbToGain(p(Volume));
    float* L = io.channel(0);
    float* R = io.channel(1);
    for (auto& v : voices_) {
        if (v.kind == Kind::None) continue;
        for (int i = start; i < end && v.kind != Kind::None; ++i) {
            const float s = renderVoice(v, dt) * vol;
            L[i] += s * v.panL;
            R[i] += s * v.panR;
        }
    }
}

} // namespace roy
