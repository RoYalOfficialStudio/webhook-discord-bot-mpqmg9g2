#include "instruments/Sampler.h"

#include <algorithm>
#include <cmath>

namespace roy {

json zoneToJson(const SamplerZone& z) {
    return {{"assetId", z.assetId}, {"root", z.rootNote}, {"lo", z.lowNote}, {"hi", z.highNote}, {"loVel", z.lowVel},
            {"hiVel", z.highVel}, {"start", z.start}, {"end", z.end}, {"loop", z.loopMode}, {"loopStart", z.loopStart},
            {"loopEnd", z.loopEnd}, {"oneShot", z.oneShot}, {"reverse", z.reverse}, {"gainDb", z.gainDb}, {"tune", z.tune},
            {"pan", z.pan}, {"choke", z.chokeGroup}};
}

SamplerZone zoneFromJson(const json& j) {
    SamplerZone z;
    z.assetId = j.value("assetId", "");
    z.rootNote = j.value("root", 60);
    z.lowNote = j.value("lo", 0);
    z.highNote = j.value("hi", 127);
    z.lowVel = j.value("loVel", 1);
    z.highVel = j.value("hiVel", 127);
    z.start = j.value("start", int64_t{0});
    z.end = j.value("end", int64_t{-1});
    z.loopMode = j.value("loop", 0);
    z.loopStart = j.value("loopStart", int64_t{0});
    z.loopEnd = j.value("loopEnd", int64_t{-1});
    z.oneShot = j.value("oneShot", false);
    z.reverse = j.value("reverse", false);
    z.gainDb = j.value("gainDb", 0.0f);
    z.tune = j.value("tune", 0.0f);
    z.pan = j.value("pan", 0.0f);
    z.chokeGroup = j.value("choke", 0);
    return z;
}

RoySampler::RoySampler()
    : Instrument({{"attack", "Attack", 0, 5, 0.001f, "s"},
                  {"decay", "Decay", 0.001f, 5, 0.1f, "s"},
                  {"sustain", "Sustain", 0, 1, 1.0f},
                  {"release", "Release", 0.001f, 10, 0.05f, "s"},
                  {"volume", "Volume", -60, 12, 0, "dB"},
                  {"bendRange", "Bend Range", 0, 24, 2, "st", 25},
                  {"velocitySens", "Velocity Sensitivity", 0, 1, 1}}) {}

void RoySampler::prepare(double sr, int maxBlock) {
    Processor::prepare(sr, maxBlock);
    reset();
}

void RoySampler::reset() {
    for (auto& v : voices_) v = Voice{};
    bend_ = 0;
}

json RoySampler::saveState() const {
    json j = Processor::saveState();
    j["zones"] = json::array();
    for (auto& z : zones_) j["zones"].push_back(zoneToJson(z));
    return j;
}

void RoySampler::loadState(const json& state) {
    Processor::loadState(state);
    zones_.clear();
    if (state.is_object() && state.contains("zones") && state["zones"].is_array())
        for (auto& z : state["zones"]) zones_.push_back(zoneFromJson(z));
}

std::vector<std::string> RoySampler::requiredAssets() const {
    std::vector<std::string> ids;
    for (auto& z : zones_)
        if (!z.assetId.empty() && std::find(ids.begin(), ids.end(), z.assetId) == ids.end()) ids.push_back(z.assetId);
    return ids;
}

void RoySampler::setAsset(const std::string& id, std::shared_ptr<const AudioData> data) { data_[id] = std::move(data); }

const AudioData* RoySampler::dataFor(const SamplerZone& z) const {
    auto it = data_.find(z.assetId);
    return it == data_.end() || !it->second ? nullptr : it->second.get();
}

int RoySampler::activeVoices() const {
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    return n;
}

void RoySampler::beginBlock(int) noexcept {
    for (auto& v : voices_) v.env.set(sampleRate_, p(Attack), p(Decay), p(Sustain), p(Release));
}

void RoySampler::handleEvent(const NoteEvent& e) noexcept {
    if (e.type == NoteEvent::PitchBend) {
        bend_ = std::clamp(e.value, -1.0f, 1.0f) * p(PitchBendRange);
        return;
    }
    if (e.type == NoteEvent::AllNotesOff) {
        for (auto& v : voices_)
            if (v.active && !(v.zone && v.zone->oneShot)) {
                v.env.noteOff();
                v.released = true;
            }
        return;
    }
    if (e.type == NoteEvent::NoteOff || (e.type == NoteEvent::NoteOn && e.velocity <= 0)) {
        for (auto& v : voices_)
            if (v.active && v.note == e.note && !v.released && !(v.zone && v.zone->oneShot)) {
                v.env.noteOff();
                v.released = true;
            }
        return;
    }
    if (e.type != NoteEvent::NoteOn) return;
    const int vel127 = std::clamp(static_cast<int>(std::lround(e.velocity * 127.0f)), 1, 127);
    for (const auto& z : zones_) {
        if (e.note < z.lowNote || e.note > z.highNote || vel127 < z.lowVel || vel127 > z.highVel) continue;
        const AudioData* d = dataFor(z);
        if (!d || d->numFrames <= 1) continue;
        if (z.chokeGroup > 0)
            for (auto& v : voices_)
                if (v.active && v.zone && v.zone->chokeGroup == z.chokeGroup) v.active = false;
        Voice* slot = nullptr;
        for (auto& v : voices_)
            if (!v.active) { slot = &v; break; }
        if (!slot) {
            slot = &voices_[0];
            for (auto& v : voices_)
                if (v.age < slot->age) slot = &v;
        }
        Voice& v = *slot;
        v.active = true;
        v.released = false;
        v.note = e.note;
        v.zone = &z;
        v.data = d;
        v.age = ++counter_;
        const int64_t end = z.end < 0 ? d->numFrames : std::min(z.end, d->numFrames);
        v.dir = z.reverse ? -1 : 1;
        v.pos = z.reverse ? static_cast<double>(end - 1) : static_cast<double>(std::clamp<int64_t>(z.start, 0, end - 1));
        const double semis = e.note - z.rootNote + z.tune + e.detune;
        v.inc = std::pow(2.0, semis / 12.0) * d->sampleRate / sampleRate_;
        const float vs = p(VelocitySens);
        v.gain = dbToGain(z.gainDb) * (1.0f - vs + vs * e.velocity);
        panGains(std::clamp(z.pan + e.pan, -1.0f, 1.0f), v.panL, v.panR);
        v.env.set(sampleRate_, p(Attack), p(Decay), p(Sustain), p(Release));
        v.env.noteOn();
    }
}

void RoySampler::render(const AudioBlock& io, int start, int end) noexcept {
    float* L = io.channel(0);
    float* R = io.channel(1);
    const float vol = dbToGain(p(Volume));
    const double bendRatio = std::pow(2.0, bend_ / 12.0);
    for (auto& v : voices_) {
        if (!v.active) continue;
        const AudioData& d = *v.data;
        const SamplerZone& z = *v.zone;
        const int64_t sEnd = z.end < 0 ? d.numFrames : std::min(z.end, d.numFrames);
        const int64_t sStart = std::clamp<int64_t>(z.start, 0, sEnd - 1);
        const bool looping = z.loopMode > 0 && !v.released;
        const int64_t lStart = std::clamp<int64_t>(z.loopStart, sStart, sEnd - 1);
        const int64_t lEnd = z.loopEnd < 0 ? sEnd : std::clamp<int64_t>(z.loopEnd, lStart + 1, sEnd);
        const int rightCh = d.numChannels > 1 ? 1 : 0;
        for (int i = start; i < end; ++i) {
            // cubic (Catmull-Rom) interpolation
            const int64_t ip = static_cast<int64_t>(std::floor(v.pos));
            const float f = static_cast<float>(v.pos - static_cast<double>(ip));
            auto cr = [&](int ch) {
                const float y0 = d.sample(ch, ip - 1), y1 = d.sample(ch, ip), y2 = d.sample(ch, ip + 1), y3 = d.sample(ch, ip + 2);
                return y1 + 0.5f * f * (y2 - y0 + f * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + f * (3.0f * (y1 - y2) + y3 - y0)));
            };
            const float a = v.env.next() * v.gain * vol;
            L[i] += cr(0) * a * v.panL;
            R[i] += cr(rightCh) * a * v.panR;
            v.pos += v.inc * bendRatio * v.dir;
            if (looping && !z.reverse) {
                if (z.loopMode == 1 && v.pos >= static_cast<double>(lEnd)) v.pos -= static_cast<double>(lEnd - lStart);
                if (z.loopMode == 2) {
                    if (v.dir > 0 && v.pos >= static_cast<double>(lEnd - 1)) v.dir = -1;
                    else if (v.dir < 0 && v.pos <= static_cast<double>(lStart)) v.dir = 1;
                }
            }
            const bool stillLooping = z.loopMode > 0 && !v.released && !z.reverse;
            if ((!stillLooping && (v.pos >= static_cast<double>(sEnd - 1) || v.pos < static_cast<double>(sStart))) || !v.env.active()) {
                v.active = false;
                break;
            }
        }
    }
}

} // namespace roy
