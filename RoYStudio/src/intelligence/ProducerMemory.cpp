#include "intelligence/ProducerMemory.h"
#include "core/Files.h"
#include "core/Math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace roy::memory {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool roleIs(const Track& t, std::initializer_list<const char*> words) {
    const std::string r = lower(t.role + " " + t.name);
    for (auto* w : words)
        if (r.find(w) != std::string::npos) return true;
    return false;
}
// Effective fader gain of a track: channel + its bus (one level).
double effectiveGainDb(const Project& p, const Track& t) {
    const MixerChannel* c = p.findChannel(t.channelId);
    if (!c) return 0;
    double g = c->gainDb;
    if (!c->outputChannelId.empty())
        if (const MixerChannel* b = p.findChannel(c->outputChannelId)) g += b->gainDb;
    return g;
}
std::optional<double> meanGain(const Project& p, std::initializer_list<const char*> words, bool invert = false) {
    double sum = 0;
    int n = 0;
    for (auto& t : p.tracks) {
        const bool m = roleIs(t, words);
        if (m != invert) {
            sum += dbToGain(effectiveGainDb(p, t));
            ++n;
        }
    }
    if (!n) return std::nullopt;
    return gainToDb(sum / n);
}
json toJson(const std::vector<Preference>& v) {
    json j = json::object();
    for (auto& p : v) j[p.key] = {{"value", p.value}, {"description", p.description}, {"source", p.source}};
    return j;
}
std::vector<Preference> fromJson(const json& j, const std::string& defSource) {
    std::vector<Preference> v;
    if (!j.is_object()) return v;
    for (auto& [k, e] : j.items()) {
        if (!e.is_object()) continue;
        v.push_back({k, e.value("value", 0.0), e.value("description", ""), e.value("source", defSource)});
    }
    return v;
}
} // namespace

void ProducerMemory::loadFromProject(const Project& p) { prefs_ = fromJson(p.producerMemory.value("preferences", json::object()), "project"); }

void ProducerMemory::storeToProject(Project& p) const { p.producerMemory["preferences"] = toJson(prefs_); }

bool ProducerMemory::loadGlobal(const std::filesystem::path& file) {
    auto text = files::readAll(file);
    if (!text) return false;
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) return false;
    for (auto& g : fromJson(j.value("preferences", json::object()), "global"))
        if (!get(g.key)) prefs_.push_back(g); // project preferences win
    return true;
}

bool ProducerMemory::saveGlobal(const std::filesystem::path& file) const {
    return files::atomicWrite(file, json{{"format", "roy-producer-memory"}, {"version", 1}, {"preferences", toJson(prefs_)}}.dump(2));
}

void ProducerMemory::set(const std::string& key, double value, const std::string& description, const std::string& source) {
    for (auto& p : prefs_)
        if (p.key == key) {
            p.value = value;
            if (!description.empty()) p.description = description;
            p.source = source;
            return;
        }
    prefs_.push_back({key, value, description, source});
}

std::optional<Preference> ProducerMemory::get(const std::string& key) const {
    for (auto& p : prefs_)
        if (p.key == key) return p;
    return std::nullopt;
}

bool ProducerMemory::reset(const std::string& key) { return std::erase_if(prefs_, [&](auto& p) { return p.key == key; }) > 0; }
void ProducerMemory::resetAll() { prefs_.clear(); }

std::optional<double> ProducerMemory::vocalVsMusicDb(const Project& p) {
    auto v = meanGain(p, {"vocal", "vox", "lead"});
    auto m = meanGain(p, {"vocal", "vox", "lead", "adlib", "double", "harmony"}, true);
    if (!v || !m) return std::nullopt;
    return *v - *m;
}

std::optional<double> ProducerMemory::mainVocalPan(const Project& p) {
    for (auto& t : p.tracks)
        if (roleIs(t, {"lead", "main vocal", "vocal"}) && !roleIs(t, {"adlib", "double", "harmony"}))
            if (auto* c = p.findChannel(t.channelId)) return c->pan;
    return std::nullopt;
}

std::optional<double> ProducerMemory::adlibWidth(const Project& p) {
    double sum = 0;
    int n = 0;
    for (auto& t : p.tracks)
        if (roleIs(t, {"adlib"}))
            if (auto* c = p.findChannel(t.channelId)) {
                sum += std::fabs(c->pan) + (c->width - 1.0f) * 0.5;
                ++n;
            }
    if (!n) return std::nullopt;
    return sum / n;
}

std::optional<double> ProducerMemory::bassVsDrumsDb(const Project& p) {
    auto b = meanGain(p, {"808", "bass"});
    auto d = meanGain(p, {"drum", "kick", "snare", "beat"});
    if (!b || !d) return std::nullopt;
    return *b - *d;
}

void ProducerMemory::learnFromProject(const Project& p) {
    if (auto v = vocalVsMusicDb(p)) set("vocal_vs_music_db", *v, "Lead vocal level relative to the music (fader balance, dB)", "learned");
    if (auto v = mainVocalPan(p)) set("main_vocal_pan", *v, "Main vocal pan position (-1..1)", "learned");
    if (auto v = adlibWidth(p)) set("adlib_width", *v, "Adlib spread (|pan| + extra width)", "learned");
    if (auto v = bassVsDrumsDb(p)) set("808_vs_drums_db", *v, "808/bass level relative to the drums (dB)", "learned");
}

std::vector<Hint> ProducerMemory::hints(const Project& p, double tol) const {
    std::vector<Hint> out;
    auto check = [&](const std::string& key, std::optional<double> cur, double scaleTol, const char* what, const char* unit) {
        auto pref = get(key);
        if (!pref || !cur) return;
        if (std::fabs(*cur - pref->value) <= tol * scaleTol) return;
        out.push_back({key,
                       std::format("{} is {:.1f}{} here, your preference is {:.1f}{} (preference, not a rule)", what, *cur, unit, pref->value, unit),
                       *cur, pref->value});
    };
    check("vocal_vs_music_db", vocalVsMusicDb(p), 1.0, "Vocal vs music", " dB");
    check("808_vs_drums_db", bassVsDrumsDb(p), 1.0, "808 vs drums", " dB");
    check("main_vocal_pan", mainVocalPan(p), 0.05, "Main vocal pan", "");
    check("adlib_width", adlibWidth(p), 0.1, "Adlib width", "");
    return out;
}

} // namespace roy::memory
