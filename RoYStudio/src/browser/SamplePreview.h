#pragma once
// Browser audition: decode (cached), optional tempo sync to the project, play through
// the engine's PreviewPlayer. Also the library categories used by the browser.
#include "audio/AudioEngine.h"

#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace roy::browser {

namespace fs = std::filesystem;

enum class TempoMode { Original, Project };

struct PreviewOptions {
    float gain = 0.8f;          // linear
    TempoMode tempo = TempoMode::Original;
    double projectBpm = 120.0;
    bool loop = false;          // loops repeat; one-shots play once
};

struct PreviewInfo {
    bool ok = false;
    std::string error;
    double seconds = 0;
    double sourceBpm = 0;       // 0 = unknown
    std::string bpmSource;      // "filename" | "analysis" | ""
    double stretch = 1.0;       // applied time-stretch ratio (tempo sync)
    bool isLoop = false;
};

// "Kick_128bpm.wav", "loop 90 BPM", "trap_140_Am.wav" -> 128 / 90 / 140 (60..220), else 0.
double bpmFromFileName(const std::string& name);
// Guess whether a file is a loop (name contains "loop" or bpm, or length ~ whole bars).
bool looksLikeLoop(const std::string& name, double seconds, double bpm);

class Previewer {
public:
    explicit Previewer(AudioEngine& engine) : engine_(engine) {}
    PreviewInfo preview(const fs::path& file, const PreviewOptions& o);
    void stop();
    bool playing() const { return engine_.preview().isPlaying(); }
    const fs::path& current() const { return current_; }
    void setGain(float g) { engine_.preview().setGain(g); }
    void collect() { engine_.preview().collect(engine_.processedBlocks()); }

private:
    struct Cached { fs::path path; std::shared_ptr<const AudioData> data; double bpm; std::string bpmSource; };
    AudioEngine& engine_;
    std::list<Cached> cache_; // most recent first, max 16 files
    fs::path current_;
};

// Library categories shown in the browser: <user dir>/Library/<Category>.
std::vector<std::string> categories(); // Samples, Drums, 808, Loops, Vocals
fs::path categoryFolder(const std::string& category);

} // namespace roy::browser
