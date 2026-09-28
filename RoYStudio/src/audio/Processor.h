#pragma once
// Base class for everything that processes audio in the mixer graph:
// built-in RoY effects, instruments and hosted plugins.
//
// Contract:
//  * prepare()/reset()/saveState()/loadState() run on the message thread
//    while the processor is NOT part of a live graph (or the engine is stopped).
//  * process() runs on the audio thread: no allocation, no locks, no I/O.
//  * Parameters are std::atomic<float> so the UI and automation can change
//    them at any time without locking.
//  * All processing is stereo (2 channels) inside the mixer.
#include "core/AudioBuffer.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace roy {

using json = nlohmann::json;
struct AudioData;

struct ParamInfo {
    std::string id;
    std::string name;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    std::string unit;
    int steps = 0; // 0 = continuous, 2 = toggle, n = choice
};

struct NoteEvent {
    enum Type : uint8_t { NoteOn, NoteOff, AllNotesOff, PitchBend, Controller };
    int offset = 0;       // sample offset inside the current block
    Type type = NoteOn;
    uint8_t channel = 0;
    int16_t note = 60;
    float velocity = 1.0f; // 0..1
    float value = 0.0f;    // pitch bend (-1..1) / controller (0..1)
    int16_t controller = 0;
    float glideTo = -1.0f; // optional: slide target note (808 slides), <0 = none
    float pan = 0.0f;      // per-note pan (-1..1), used by drum voices
    float detune = 0.0f;   // per-note pitch offset in semitones
    bool slide = false;    // legato slide into this note (808)
};

class Processor {
public:
    explicit Processor(std::vector<ParamInfo> params = {});
    virtual ~Processor() = default;
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    virtual std::string typeId() const = 0;
    virtual std::string displayName() const { return typeId(); }
    virtual bool isInstrument() const { return false; }
    virtual bool wantsSidechain() const { return false; }

    virtual void prepare(double sampleRate, int maxBlockSize) {
        sampleRate_ = sampleRate;
        maxBlock_ = maxBlockSize;
    }
    virtual void reset() {}
    // Effects process `io` in place. Instruments ADD their output to `io`.
    virtual void process(const AudioBlock& io, const AudioBlock* sidechain, const NoteEvent* events,
                         int numEvents) noexcept = 0;
    virtual int latencySamples() const { return 0; }
    virtual double tailSeconds() const { return 0.0; }
    // Project tempo (BPM) for tempo-synced processors. Message thread; stored atomically by implementations.
    virtual void setHostTempo(double bpm) {}

    // ---- parameters -------------------------------------------------------
    int numParams() const { return static_cast<int>(info_.size()); }
    const ParamInfo& paramInfo(int i) const { return info_[static_cast<size_t>(i)]; }
    const std::vector<ParamInfo>& params() const { return info_; }
    int findParam(const std::string& id) const;
    float getParam(int i) const { return values_[static_cast<size_t>(i)].load(std::memory_order_relaxed); }
    float getParam(const std::string& id) const;
    void setParam(int i, float v) noexcept;
    bool setParam(const std::string& id, float v);

    // ---- state ------------------------------------------------------------
    // Default: all parameters by id. Subclasses may add opaque state.
    virtual json saveState() const;
    virtual void loadState(const json& state);

    // Audio assets the processor needs (e.g. sampler zones). The runtime loads
    // them and calls setAsset() on the message thread before the processor goes live.
    virtual std::vector<std::string> requiredAssets() const { return {}; }
    virtual void setAsset(const std::string& assetId, std::shared_ptr<const AudioData> data) {}

    double sampleRate() const { return sampleRate_; }
    int maxBlock() const { return maxBlock_; }

protected:
    float p(int i) const { return values_[static_cast<size_t>(i)].load(std::memory_order_relaxed); }

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;

private:
    std::vector<ParamInfo> info_;
    std::unique_ptr<std::atomic<float>[]> values_;
};

// Registry of processor types ("roy.eq", "roy.compressor", ...).
class ProcessorFactory {
public:
    using Creator = std::function<std::unique_ptr<Processor>()>;
    static ProcessorFactory& instance();
    void add(const std::string& typeId, const std::string& displayName, bool instrument, Creator c);
    std::unique_ptr<Processor> create(const std::string& typeId) const;
    bool has(const std::string& typeId) const;
    struct Entry { std::string typeId, displayName; bool instrument; Creator create; };
    std::vector<Entry> entries() const;

    // External creators (e.g. plugin host) can claim prefixes like "clap:".
    using PrefixCreator = std::function<std::unique_ptr<Processor>(const std::string& typeId)>;
    void addPrefix(const std::string& prefix, PrefixCreator c);

private:
    std::map<std::string, Entry> entries_;
    std::map<std::string, PrefixCreator> prefixes_;
};

// Registers all built-in RoY processors. Idempotent.
void registerBuiltinProcessors();

} // namespace roy
