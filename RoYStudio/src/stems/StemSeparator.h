#pragma once
// STEM SYSTEM architecture.
// A StemSeparator turns a mix into named stems. Separators are registered by id;
// better engines (e.g. an ML model running out-of-process) can be added later
// without touching callers. The original file is never modified.
//
// Built-in: "roy.dsp-basic" (BasicStemSeparator) - classic DSP (HPSS + band split +
// centre mask). It is fast and dependency-free but has HEAVY BLEED between stems; its
// quality report says so. Sum of stems == input (exact reconstruction).
//
// "roy.external" (AdvancedStemSeparator) - adapter for a high-quality separation engine
// (e.g. Demucs, Spleeter, a UVR command line) that the USER installs and configures in
// <user data>/stem_engine.json. RoY ships no model and never starts an executable that
// was not configured explicitly. The engine runs out of process (a crash or hang cannot
// take RoY down) with a timeout; its stems are validated and the residual becomes "Other"
// so the stems always sum to the input. Without configuration it reports unavailable.
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

class IStemSeparator {
public:
    virtual ~IStemSeparator() = default;
    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual std::vector<std::string> stemNames() const = 0;
    virtual bool available(std::string* why = nullptr) const = 0;
    // Empty result (no stems) + quality.warnings on failure or cancel.
    virtual StemResult separate(const Channels& mix, double sampleRate, const std::function<bool(double)>& progress = {}) = 0;
};
using StemSeparator = IStemSeparator; // earlier name

class BasicStemSeparator : public IStemSeparator {
public:
    std::string id() const override { return "roy.dsp-basic"; }
    std::string description() const override;
    std::vector<std::string> stemNames() const override { return {"Vocals", "Drums", "Bass", "Other"}; }
    bool available(std::string*) const override { return true; }
    StemResult separate(const Channels& mix, double sampleRate, const std::function<bool(double)>& progress = {}) override;
};

// Engine configuration (stem_engine.json):
// { "command": "C:/Tools/demucs/demucs.exe",
//   "args": ["-n", "htdemucs", "-o", "{out}", "{in}"],
//   "stems": {"Vocals": "{out}/htdemucs/{name}/vocals.wav", "Drums": "...", "Bass": "...", "Other": "..."},
//   "timeoutSec": 1800, "label": "Demucs htdemucs" }
// Placeholders: {in} input WAV, {out} work folder, {name} input file stem.
struct ExternalEngineConfig {
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> stems; // stem name -> output path pattern
    int timeoutSec = 1800;
    std::string label;
    static bool fromJson(const std::string& text, ExternalEngineConfig& out, std::string* error = nullptr);
};

class AdvancedStemSeparator : public IStemSeparator {
public:
    // Reads <configFile> on every call (the user can install/configure an engine without restarting).
    explicit AdvancedStemSeparator(std::filesystem::path configFile);
    std::string id() const override { return "roy.external"; }
    std::string description() const override;
    std::vector<std::string> stemNames() const override;
    bool available(std::string* why = nullptr) const override;
    StemResult separate(const Channels& mix, double sampleRate, const std::function<bool(double)>& progress = {}) override;
    const std::filesystem::path& configFile() const { return config_; }

private:
    std::filesystem::path config_;
    bool load(ExternalEngineConfig& c, std::string* why) const;
};

// Quality measurements shared by all separators (reconstruction error, envelope cross-talk).
void measureQuality(const Channels& mix, StemResult& r, double sampleRate);

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
