#pragma once
// PRODUCER MEMORY: production preferences stored per project (project file) or
// explicitly in a global file chosen by the user. They are preferences, not
// rules: they only produce hints, never automatic changes. Everything can be reset.
#include "project/Project.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace roy::memory {

struct Preference {
    std::string key;         // "vocal_vs_music_db", "adlib_width", "main_vocal_pan", "808_vs_drums_db", "kick_length_ms", ...
    double value = 0;
    std::string description;
    std::string source;      // "project" | "global" | "learned"
};

struct Hint {
    std::string key;
    std::string text;
    double current = 0, preferred = 0;
};

class ProducerMemory {
public:
    // Load from / store to the project (project.producerMemory).
    void loadFromProject(const Project& p);
    void storeToProject(Project& p) const;
    // Explicit global file (only when the user saves it).
    bool loadGlobal(const std::filesystem::path& file);
    bool saveGlobal(const std::filesystem::path& file) const;

    void set(const std::string& key, double value, const std::string& description = {}, const std::string& source = "project");
    std::optional<Preference> get(const std::string& key) const;
    bool reset(const std::string& key);
    void resetAll();
    const std::vector<Preference>& all() const { return prefs_; }

    // Learns (and stores as "learned") the current mix relations of a project:
    // vocal-vs-music fader balance, main vocal pan, adlib width, 808-vs-drums balance.
    void learnFromProject(const Project& p);
    // Hints where the project differs from the preferences by more than `toleranceDb`/units.
    std::vector<Hint> hints(const Project& p, double tolerance = 2.0) const;

    // Measured relations used by learn/hints (fader-based; exposed for tests/UI).
    static std::optional<double> vocalVsMusicDb(const Project& p);
    static std::optional<double> mainVocalPan(const Project& p);
    static std::optional<double> adlibWidth(const Project& p);
    static std::optional<double> bassVsDrumsDb(const Project& p);

private:
    std::vector<Preference> prefs_;
};

} // namespace roy::memory
