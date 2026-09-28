#pragma once
// RoY Sampler: multi-zone sampler with key / velocity mapping, root note,
// start/end, loop (forward / ping-pong), one-shot, reverse, per-zone gain and
// tune, ADSR, 32 voices with cubic interpolation.
// Zones live in the processor state ("zones"), sample data is provided by the
// runtime through setAsset().
#include "audio/RenderGraph.h"
#include "dsp/Filters.h"
#include "instruments/Instrument.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace roy {

struct SamplerZone {
    std::string assetId;
    int rootNote = 60;
    int lowNote = 0, highNote = 127;
    int lowVel = 1, highVel = 127;
    int64_t start = 0;
    int64_t end = -1;          // -1 = end of sample
    int loopMode = 0;          // 0 off, 1 forward, 2 ping-pong
    int64_t loopStart = 0, loopEnd = -1;
    bool oneShot = false;      // ignore note-off
    bool reverse = false;
    float gainDb = 0.0f;
    float tune = 0.0f;         // semitones
    float pan = 0.0f;
    int chokeGroup = 0;        // >0: zones in the same group cut each other (pads)
};
json zoneToJson(const SamplerZone& z);
SamplerZone zoneFromJson(const json& j);

class RoySampler : public Instrument {
public:
    enum P { Attack, Decay, Sustain, Release, Volume, PitchBendRange, VelocitySens, NumParams };
    RoySampler();
    std::string typeId() const override { return "roy.sampler"; }
    std::string displayName() const override { return "RoY Sampler"; }
    void prepare(double sr, int maxBlock) override;
    void reset() override;
    double tailSeconds() const override { return 5.0; }
    json saveState() const override;
    void loadState(const json& state) override;
    std::vector<std::string> requiredAssets() const override;
    void setAsset(const std::string& assetId, std::shared_ptr<const AudioData> data) override;

    // Message thread, before the processor is live.
    void setZones(std::vector<SamplerZone> zones) { zones_ = std::move(zones); }
    const std::vector<SamplerZone>& zones() const { return zones_; }
    int activeVoices() const;

protected:
    void beginBlock(int numFrames) noexcept override;
    void render(const AudioBlock& io, int start, int end) noexcept override;
    void handleEvent(const NoteEvent& e) noexcept override;

private:
    struct Voice {
        bool active = false;
        int note = -1;
        const SamplerZone* zone = nullptr;
        const AudioData* data = nullptr;
        double pos = 0, inc = 1;
        int dir = 1;
        float gain = 1, panL = 1, panR = 1;
        dsp::Adsr env;
        uint64_t age = 0;
        bool released = false;
    };
    const AudioData* dataFor(const SamplerZone& z) const;

    std::vector<SamplerZone> zones_;
    std::map<std::string, std::shared_ptr<const AudioData>> data_;
    std::array<Voice, 32> voices_;
    uint64_t counter_ = 0;
    double bend_ = 0;
};

} // namespace roy
