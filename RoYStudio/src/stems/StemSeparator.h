#pragma once
// STEM SYSTEM architecture.
// A StemSeparator turns a mix into named stems. Separators are registered by id;
// better engines (e.g. an ML model running out-of-process) can be added later
// without touching callers. The original file is never modified.
//
// Built-in: "roy.dsp-basic" - classic DSP (HPSS + band split + centre mask).
// It is fast and dependency-free but has HEAVY BLEED between stems; its
// quality report says so. Sum of stems == input (exact reconstruction).
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace roy::stems {

using Channels = std::vector<std::vector<float>>;

struct StemQuality {
    std::string method;
    double reconstructionErrorDb = -300; // energy of (sum of stems - input) relative to input
    double crossTalk = 0;                // mean |correlation| of stem energy envelopes (0 = independent)
    std::vector<std::string> warnings;   // artefacts / limitations, always filled in honestly
};

struct StemResult {
    std::vector<std::string> names;
    std::vector<Channels> stems;
    StemQuality quality;
    const Channels* find(const std::string& name) const;
};

class StemSeparator {
public:
    virtual ~StemSeparator() = default;
    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual std::vector<std::string> stemNames() const = 0;
    virtual bool available(std::string* why = nullptr) const = 0;
    virtual StemResult separate(const Channels& mix, double sampleRate, const std::function<bool(double)>& progress = {}) = 0;
};

class StemRegistry {
public:
    static StemRegistry& instance();
    void add(std::unique_ptr<StemSeparator> s);
    StemSeparator* find(const std::string& id) const;
    std::vector<StemSeparator*> list() const;

private:
    std::map<std::string, std::unique_ptr<StemSeparator>> separators_;
};

void registerBuiltinSeparators();

// Signal-to-distortion ratio (dB) of an estimate against a known reference (for evaluation).
double sdr(const Channels& reference, const Channels& estimate);

// Writes stems as <folder>/<base>_<Stem>.wav (never overwrites). Returns file paths.
std::vector<std::filesystem::path> writeStems(const StemResult& r, double sampleRate, const std::filesystem::path& folder,
                                              const std::string& baseName, std::string* error = nullptr);

} // namespace roy::stems
