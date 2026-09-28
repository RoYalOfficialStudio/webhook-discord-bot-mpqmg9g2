#include "project/ProjectIO.h"
#include "core/Files.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <regex>
#include <set>

namespace roy {

namespace fs = std::filesystem;

namespace {

// ---- enum helpers ------------------------------------------------------------
const char* trackTypeId(TrackType t) {
    switch (t) {
    case TrackType::Audio: return "audio";
    case TrackType::Midi: return "midi";
    case TrackType::Beat: return "beat";
    }
    return "audio";
}
TrackType trackTypeFrom(const std::string& s) {
    if (s == "midi") return TrackType::Midi;
    if (s == "beat") return TrackType::Beat;
    return TrackType::Audio;
}
const char* channelKindId(ChannelKind k) {
    switch (k) {
    case ChannelKind::Track: return "track";
    case ChannelKind::Bus: return "bus";
    case ChannelKind::Master: return "master";
    }
    return "track";
}
ChannelKind channelKindFrom(const std::string& s) {
    if (s == "bus") return ChannelKind::Bus;
    if (s == "master") return ChannelKind::Master;
    return ChannelKind::Track;
}

template <typename T>
T get(const json& j, const char* key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

// ---- structs -> json ---------------------------------------------------------
json toJ(const AudioAsset& a) {
    return {{"id", a.id}, {"path", a.path}, {"originalName", a.originalName}, {"kind", a.kind},
            {"sampleRate", a.sampleRate}, {"channels", a.channels}, {"frames", a.frames}, {"sha256", a.sha256},
            {"createdAt", a.createdAt}};
}
AudioAsset assetFrom(const json& j) {
    AudioAsset a;
    a.id = get<std::string>(j, "id", files::newId());
    a.path = get<std::string>(j, "path", "");
    a.originalName = get<std::string>(j, "originalName", "");
    a.kind = get<std::string>(j, "kind", "import");
    a.sampleRate = get<double>(j, "sampleRate", 48000.0);
    a.channels = get<int>(j, "channels", 2);
    a.frames = get<int64_t>(j, "frames", 0);
    a.sha256 = get<std::string>(j, "sha256", "");
    a.createdAt = get<std::string>(j, "createdAt", "");
    return a;
}

json toJ(const AudioClip& c) {
    return {{"id", c.id}, {"assetId", c.assetId}, {"name", c.name}, {"color", c.color}, {"startBeat", c.startBeat},
            {"lengthBeats", c.lengthBeats}, {"sourceOffsetSec", c.sourceOffsetSec}, {"stretch", c.stretch},
            {"pitchSemitones", c.pitchSemitones}, {"gainDb", c.gainDb}, {"muted", c.muted}, {"locked", c.locked},
            {"reversed", c.reversed}, {"groupId", c.groupId}, {"fadeInBeats", c.fadeInBeats},
            {"fadeOutBeats", c.fadeOutBeats}, {"fadeInCurve", static_cast<int>(c.fadeInCurve)},
            {"fadeOutCurve", static_cast<int>(c.fadeOutCurve)}};
}
AudioClip audioClipFrom(const json& j) {
    AudioClip c;
    c.id = get<std::string>(j, "id", files::newId());
    c.assetId = get<std::string>(j, "assetId", "");
    c.name = get<std::string>(j, "name", "");
    c.color = get<uint32_t>(j, "color", c.color);
    c.startBeat = get<double>(j, "startBeat", 0.0);
    c.lengthBeats = get<double>(j, "lengthBeats", 4.0);
    c.sourceOffsetSec = get<double>(j, "sourceOffsetSec", 0.0);
    c.stretch = get<double>(j, "stretch", 1.0);
    c.pitchSemitones = get<double>(j, "pitchSemitones", 0.0);
    c.gainDb = get<float>(j, "gainDb", 0.0f);
    c.muted = get<bool>(j, "muted", false);
    c.locked = get<bool>(j, "locked", false);
    c.reversed = get<bool>(j, "reversed", false);
    c.groupId = get<std::string>(j, "groupId", "");
    c.fadeInBeats = get<double>(j, "fadeInBeats", 0.0);
    c.fadeOutBeats = get<double>(j, "fadeOutBeats", 0.0);
    c.fadeInCurve = static_cast<FadeCurve>(std::clamp(get<int>(j, "fadeInCurve", 1), 0, 3));
    c.fadeOutCurve = static_cast<FadeCurve>(std::clamp(get<int>(j, "fadeOutCurve", 1), 0, 3));
    return c;
}

json toJ(const MidiNote& n) {
    return {{"p", n.pitch}, {"v", n.velocity}, {"s", n.startBeat}, {"l", n.lengthBeats}, {"c", n.channel}, {"m", n.muted}, {"sl", n.slide}};
}
MidiNote noteFrom(const json& j) {
    MidiNote n;
    n.pitch = std::clamp(get<int>(j, "p", 60), 0, 127);
    n.velocity = std::clamp(get<int>(j, "v", 100), 1, 127);
    n.startBeat = get<double>(j, "s", 0.0);
    n.lengthBeats = get<double>(j, "l", 1.0);
    n.channel = get<int>(j, "c", 0);
    n.muted = get<bool>(j, "m", false);
    n.slide = get<bool>(j, "sl", false);
    return n;
}

json toJ(const MidiClip& c) {
    json notes = json::array();
    for (auto& n : c.notes) notes.push_back(toJ(n));
    return {{"id", c.id}, {"name", c.name}, {"color", c.color}, {"startBeat", c.startBeat}, {"lengthBeats", c.lengthBeats},
            {"loopLengthBeats", c.loopLengthBeats}, {"muted", c.muted}, {"locked", c.locked}, {"groupId", c.groupId},
            {"notes", notes}};
}
MidiClip midiClipFrom(const json& j) {
    MidiClip c;
    c.id = get<std::string>(j, "id", files::newId());
    c.name = get<std::string>(j, "name", "");
    c.color = get<uint32_t>(j, "color", c.color);
    c.startBeat = get<double>(j, "startBeat", 0.0);
    c.lengthBeats = get<double>(j, "lengthBeats", 4.0);
    c.loopLengthBeats = get<double>(j, "loopLengthBeats", 0.0);
    c.muted = get<bool>(j, "muted", false);
    c.locked = get<bool>(j, "locked", false);
    c.groupId = get<std::string>(j, "groupId", "");
    if (auto it = j.find("notes"); it != j.end() && it->is_array())
        for (auto& n : *it) c.notes.push_back(noteFrom(n));
    return c;
}

json toJ(const Step& s) {
    return {{"on", s.on}, {"vel", s.velocity}, {"pan", s.pan}, {"pitch", s.pitch}, {"prob", s.probability},
            {"micro", s.microTiming}, {"flam", s.flam}, {"roll", s.roll}};
}
Step stepFrom(const json& j) {
    Step s;
    s.on = get<bool>(j, "on", false);
    s.velocity = get<float>(j, "vel", 0.8f);
    s.pan = get<float>(j, "pan", 0.0f);
    s.pitch = get<float>(j, "pitch", 0.0f);
    s.probability = get<float>(j, "prob", 1.0f);
    s.microTiming = get<float>(j, "micro", 0.0f);
    s.flam = get<bool>(j, "flam", false);
    s.roll = get<int>(j, "roll", 0);
    return s;
}

json toJ(const Pattern& p) {
    json rows = json::array();
    for (auto& r : p.rows) {
        json steps = json::array();
        for (auto& s : r.steps) steps.push_back(toJ(s));
        rows.push_back({{"id", r.id}, {"name", r.name}, {"voice", r.voice}, {"sampleAssetId", r.sampleAssetId},
                        {"note", r.note}, {"muted", r.muted}, {"solo", r.solo}, {"volume", r.volume}, {"pan", r.pan},
                        {"pitch", r.pitch}, {"steps", steps}});
    }
    return {{"id", p.id}, {"name", p.name}, {"numSteps", p.numSteps}, {"stepLengthBeats", p.stepLengthBeats},
            {"swing", p.swing}, {"rows", rows}};
}
Pattern patternFrom(const json& j) {
    Pattern p;
    p.id = get<std::string>(j, "id", files::newId());
    p.name = get<std::string>(j, "name", "Pattern");
    p.numSteps = std::clamp(get<int>(j, "numSteps", 16), 1, 256);
    p.stepLengthBeats = get<double>(j, "stepLengthBeats", 0.25);
    p.swing = get<float>(j, "swing", 0.0f);
    if (auto it = j.find("rows"); it != j.end() && it->is_array())
        for (auto& rj : *it) {
            PatternRow r;
            r.id = get<std::string>(rj, "id", files::newId());
            r.name = get<std::string>(rj, "name", "");
            r.voice = get<std::string>(rj, "voice", "kick");
            r.sampleAssetId = get<std::string>(rj, "sampleAssetId", "");
            r.note = get<int>(rj, "note", 36);
            r.muted = get<bool>(rj, "muted", false);
            r.solo = get<bool>(rj, "solo", false);
            r.volume = get<float>(rj, "volume", 0.8f);
            r.pan = get<float>(rj, "pan", 0.0f);
            r.pitch = get<float>(rj, "pitch", 0.0f);
            if (auto st = rj.find("steps"); st != rj.end() && st->is_array())
                for (auto& s : *st) r.steps.push_back(stepFrom(s));
            p.rows.push_back(r);
        }
    return p;
}

json toJ(const PatternClip& c) {
    return {{"id", c.id}, {"patternId", c.patternId}, {"startBeat", c.startBeat}, {"lengthBeats", c.lengthBeats},
            {"muted", c.muted}, {"locked", c.locked}, {"color", c.color}};
}
PatternClip patternClipFrom(const json& j) {
    PatternClip c;
    c.id = get<std::string>(j, "id", files::newId());
    c.patternId = get<std::string>(j, "patternId", "");
    c.startBeat = get<double>(j, "startBeat", 0.0);
    c.lengthBeats = get<double>(j, "lengthBeats", 4.0);
    c.muted = get<bool>(j, "muted", false);
    c.locked = get<bool>(j, "locked", false);
    c.color = get<uint32_t>(j, "color", c.color);
    return c;
}

json toJ(const PluginSlot& s) {
    return {{"id", s.id}, {"typeId", s.typeId}, {"name", s.name}, {"bypass", s.bypass}, {"state", s.state},
            {"sidechainChannelId", s.sidechainChannelId}};
}
PluginSlot slotFrom(const json& j) {
    PluginSlot s;
    s.id = get<std::string>(j, "id", files::newId());
    s.typeId = get<std::string>(j, "typeId", "");
    s.name = get<std::string>(j, "name", "");
    s.bypass = get<bool>(j, "bypass", false);
    s.state = j.contains("state") ? j["state"] : json::object();
    s.sidechainChannelId = get<std::string>(j, "sidechainChannelId", "");
    return s;
}

json toJ(const MixerChannel& c) {
    json inserts = json::array(), sends = json::array();
    for (auto& s : c.inserts) inserts.push_back(toJ(s));
    for (auto& s : c.sends)
        sends.push_back({{"id", s.id}, {"target", s.targetChannelId}, {"levelDb", s.levelDb}, {"preFader", s.preFader}, {"enabled", s.enabled}});
    json j = {{"id", c.id}, {"name", c.name}, {"kind", channelKindId(c.kind)}, {"gainDb", c.gainDb}, {"pan", c.pan},
            {"width", c.width}, {"mute", c.mute}, {"solo", c.solo}, {"phaseInvert", c.phaseInvert}, {"inserts", inserts},
            {"sends", sends}, {"output", c.outputChannelId}, {"color", c.color}};
    if (c.soloSafe) j["soloSafe"] = true;
    return j;
}
MixerChannel channelFrom(const json& j) {
    MixerChannel c;
    c.id = get<std::string>(j, "id", files::newId());
    c.name = get<std::string>(j, "name", "");
    c.kind = channelKindFrom(get<std::string>(j, "kind", "track"));
    c.gainDb = get<float>(j, "gainDb", 0.0f);
    c.pan = get<float>(j, "pan", 0.0f);
    c.width = get<float>(j, "width", 1.0f);
    c.mute = get<bool>(j, "mute", false);
    c.solo = get<bool>(j, "solo", false);
    c.soloSafe = get<bool>(j, "soloSafe", false);
    c.phaseInvert = get<bool>(j, "phaseInvert", false);
    c.outputChannelId = get<std::string>(j, "output", "");
    c.color = get<uint32_t>(j, "color", c.color);
    if (auto it = j.find("inserts"); it != j.end() && it->is_array())
        for (auto& s : *it) c.inserts.push_back(slotFrom(s));
    if (auto it = j.find("sends"); it != j.end() && it->is_array())
        for (auto& s : *it) {
            Send x;
            x.id = get<std::string>(s, "id", files::newId());
            x.targetChannelId = get<std::string>(s, "target", "");
            x.levelDb = get<float>(s, "levelDb", -6.0f);
            x.preFader = get<bool>(s, "preFader", false);
            x.enabled = get<bool>(s, "enabled", true);
            c.sends.push_back(x);
        }
    return c;
}

json toJ(const Track& t) {
    json ac = json::array(), mc = json::array(), pc = json::array(), takes = json::array(), comp = json::array();
    for (auto& c : t.audioClips) ac.push_back(toJ(c));
    for (auto& c : t.midiClips) mc.push_back(toJ(c));
    for (auto& c : t.patternClips) pc.push_back(toJ(c));
    for (auto& k : t.takes)
        takes.push_back({{"id", k.id}, {"assetId", k.assetId}, {"name", k.name}, {"lane", k.lane}, {"startBeat", k.startBeat},
                         {"lengthBeats", k.lengthBeats}, {"createdAt", k.createdAt}, {"loopPass", k.loopPass}});
    for (auto& s : t.comp) comp.push_back({{"start", s.startBeat}, {"end", s.endBeat}, {"take", s.takeId}});
    json j = {{"id", t.id}, {"name", t.name}, {"type", trackTypeId(t.type)}, {"color", t.color}, {"channelId", t.channelId},
              {"armed", t.armed}, {"monitor", t.monitor}, {"inputLeft", t.inputLeft}, {"inputRight", t.inputRight},
              {"inputGainDb", t.inputGainDb}, {"locked", t.locked}, {"role", t.role}, {"audioClips", ac},
              {"midiClips", mc}, {"patternClips", pc}, {"takes", takes}, {"comp", comp}};
    if (t.instrument) j["instrument"] = toJ(*t.instrument);
    return j;
}
Track trackFrom(const json& j) {
    Track t;
    t.id = get<std::string>(j, "id", files::newId());
    t.name = get<std::string>(j, "name", "");
    t.type = trackTypeFrom(get<std::string>(j, "type", "audio"));
    t.color = get<uint32_t>(j, "color", t.color);
    t.channelId = get<std::string>(j, "channelId", "");
    t.armed = get<bool>(j, "armed", false);
    t.monitor = get<bool>(j, "monitor", false);
    t.inputLeft = get<int>(j, "inputLeft", 0);
    t.inputRight = get<int>(j, "inputRight", -1);
    t.inputGainDb = get<float>(j, "inputGainDb", 0.0f);
    t.locked = get<bool>(j, "locked", false);
    t.role = get<std::string>(j, "role", "");
    if (auto it = j.find("instrument"); it != j.end() && it->is_object()) t.instrument = slotFrom(*it);
    if (auto it = j.find("audioClips"); it != j.end() && it->is_array())
        for (auto& c : *it) t.audioClips.push_back(audioClipFrom(c));
    if (auto it = j.find("midiClips"); it != j.end() && it->is_array())
        for (auto& c : *it) t.midiClips.push_back(midiClipFrom(c));
    if (auto it = j.find("patternClips"); it != j.end() && it->is_array())
        for (auto& c : *it) t.patternClips.push_back(patternClipFrom(c));
    if (auto it = j.find("takes"); it != j.end() && it->is_array())
        for (auto& k : *it) {
            Take x;
            x.id = get<std::string>(k, "id", files::newId());
            x.assetId = get<std::string>(k, "assetId", "");
            x.name = get<std::string>(k, "name", "");
            x.lane = get<int>(k, "lane", 0);
            x.startBeat = get<double>(k, "startBeat", 0.0);
            x.lengthBeats = get<double>(k, "lengthBeats", 0.0);
            x.createdAt = get<std::string>(k, "createdAt", "");
            x.loopPass = get<int>(k, "loopPass", 0);
            t.takes.push_back(x);
        }
    if (auto it = j.find("comp"); it != j.end() && it->is_array())
        for (auto& s : *it) t.comp.push_back({get<double>(s, "start", 0.0), get<double>(s, "end", 0.0), get<std::string>(s, "take", "")});
    return t;
}

const std::set<std::string>& knownTopLevelKeys() {
    static const std::set<std::string> k = {"format", "formatVersion", "id", "name", "author", "createdAt", "modifiedAt",
                                            "appVersion", "sampleRate", "tempo", "key", "settings", "loop", "markers",
                                            "sections", "assets", "tracks", "channels", "automation", "patterns",
                                            "presets", "producerMemory", "vocalSettings", "metadata"};
    return k;
}

} // namespace

json projectToJson(const Project& p) {
    json j;
    j["format"] = "roy-studio-project";
    j["formatVersion"] = kProjectFormatVersion;
    j["id"] = p.id;
    j["name"] = p.name;
    j["author"] = p.author;
    j["createdAt"] = p.createdAt;
    j["modifiedAt"] = p.modifiedAt;
    j["appVersion"] = p.appVersion;
    j["sampleRate"] = p.sampleRate;
    json tempo = {{"events", json::array()}, {"timeSignatures", json::array()}};
    for (auto& e : p.tempo.tempoEvents()) tempo["events"].push_back({{"beat", e.beat}, {"bpm", e.bpm}});
    for (auto& s : p.tempo.timeSignatures())
        tempo["timeSignatures"].push_back({{"bar", s.bar}, {"num", s.numerator}, {"den", s.denominator}});
    j["tempo"] = tempo;
    j["key"] = {{"root", p.key.root}, {"scale", scaleTypeId(p.key.scale)}};
    const auto& s = p.settings;
    j["settings"] = {{"metronome", s.metronome}, {"metronomeGainDb", s.metronomeGainDb}, {"countInBars", s.countInBars},
                     {"preRollBeats", s.preRollBeats}, {"punchEnabled", s.punchEnabled}, {"punchInBeat", s.punchInBeat},
                     {"punchOutBeat", s.punchOutBeat}, {"snapBeats", s.snapBeats},
                     {"wrongNoteMode", wrongNoteModeId(s.wrongNoteMode)}, {"autosaveIntervalSec", s.autosaveIntervalSec},
                     {"backupRetention", s.backupRetention}, {"neverLoseSeconds", s.neverLoseSeconds}};
    j["loop"] = {{"enabled", p.loop.enabled}, {"startBeat", p.loop.startBeat}, {"endBeat", p.loop.endBeat}};
    j["markers"] = json::array();
    for (auto& m : p.markers) j["markers"].push_back({{"id", m.id}, {"beat", m.beat}, {"name", m.name}, {"color", m.color}});
    j["sections"] = json::array();
    for (auto& x : p.sections)
        j["sections"].push_back({{"id", x.id}, {"name", x.name}, {"type", x.type}, {"startBeat", x.startBeat},
                                 {"endBeat", x.endBeat}, {"color", x.color}});
    j["assets"] = json::array();
    for (auto& a : p.assets) j["assets"].push_back(toJ(a));
    j["channels"] = json::array();
    for (auto& c : p.channels) j["channels"].push_back(toJ(c));
    j["tracks"] = json::array();
    for (auto& t : p.tracks) j["tracks"].push_back(toJ(t));
    j["automation"] = json::array();
    for (auto& a : p.automation) {
        json pts = json::array();
        for (auto& pt : a.points) {
            if (pt.curve == 0 && pt.tension == 0.0f) pts.push_back({pt.beat, pt.value}); // v1 layout
            else pts.push_back({pt.beat, pt.value, pt.curve, pt.tension});
        }
        j["automation"].push_back({{"id", a.id}, {"channelId", a.channelId}, {"slotId", a.slotId}, {"paramId", a.paramId},
                                   {"enabled", a.enabled}, {"points", pts}});
    }
    j["patterns"] = json::array();
    for (auto& pat : p.patterns) j["patterns"].push_back(toJ(pat));
    j["presets"] = p.presets;
    j["producerMemory"] = p.producerMemory;
    j["vocalSettings"] = p.vocalSettings;
    j["metadata"] = p.metadata;
    for (auto& [k, v] : p.unknown.items())
        if (!j.contains(k)) j[k] = v;
    return j;
}

bool migrateProjectJson(json& j, std::vector<std::string>* log) {
    int v = j.value("formatVersion", 0);
    if (v > kProjectFormatVersion) return false;
    // v0 (pre-release draft format): flat "bpm"/"timeSignature" fields, "mixer" array, tracks with "clips".
    if (v == 0) {
        if (j.contains("bpm") && !j.contains("tempo")) {
            j["tempo"] = {{"events", json::array({{{"beat", 0.0}, {"bpm", j["bpm"]}}})},
                          {"timeSignatures", json::array({{{"bar", 0}, {"num", j.value("timeSignatureNum", 4)}, {"den", j.value("timeSignatureDen", 4)}}})}};
            j.erase("bpm");
            j.erase("timeSignatureNum");
            j.erase("timeSignatureDen");
            if (log) log->push_back("v0->v1: moved bpm/time signature into tempo map");
        }
        if (j.contains("mixer") && !j.contains("channels")) {
            j["channels"] = j["mixer"];
            j.erase("mixer");
            if (log) log->push_back("v0->v1: renamed mixer -> channels");
        }
        if (j.contains("tracks") && j["tracks"].is_array())
            for (auto& t : j["tracks"])
                if (t.contains("clips") && !t.contains("audioClips")) {
                    t["audioClips"] = t["clips"];
                    t.erase("clips");
                    if (log) log->push_back("v0->v1: renamed track clips -> audioClips");
                }
        v = 1;
    }
    j["formatVersion"] = v;
    return true;
}

bool projectFromJson(const json& jIn, Project& p, std::string* error, std::vector<std::string>* migrationLog) {
    if (!jIn.is_object()) {
        if (error) *error = "project root is not an object";
        return false;
    }
    json j = jIn;
    const int version = j.value("formatVersion", 0);
    if (version < kProjectFormatVersion && !migrateProjectJson(j, migrationLog)) {
        if (error) *error = "migration failed";
        return false;
    }
    Project out;
    out.formatVersion = std::max(version, kProjectFormatVersion);
    out.id = get<std::string>(j, "id", files::newId());
    out.name = get<std::string>(j, "name", "Untitled");
    out.author = get<std::string>(j, "author", "");
    out.createdAt = get<std::string>(j, "createdAt", "");
    out.modifiedAt = get<std::string>(j, "modifiedAt", "");
    out.appVersion = get<std::string>(j, "appVersion", "");
    out.sampleRate = get<double>(j, "sampleRate", 48000.0);
    if (auto it = j.find("tempo"); it != j.end() && it->is_object()) {
        TempoMap tm;
        bool first = true;
        for (auto& e : it->value("events", json::array())) {
            if (first) tm.setTempo(get<double>(e, "bpm", 120.0));
            else tm.addTempoEvent(get<double>(e, "beat", 0.0), get<double>(e, "bpm", 120.0));
            first = false;
        }
        bool firstSig = true;
        for (auto& s : it->value("timeSignatures", json::array())) {
            if (firstSig) tm.setTimeSignature(get<int>(s, "num", 4), get<int>(s, "den", 4));
            else tm.addTimeSignature(get<int>(s, "bar", 0), get<int>(s, "num", 4), get<int>(s, "den", 4));
            firstSig = false;
        }
        out.tempo = tm;
    }
    if (auto it = j.find("key"); it != j.end() && it->is_object()) {
        out.key.root = std::clamp(get<int>(*it, "root", 0), 0, 11);
        out.key.scale = scaleTypeFromId(get<std::string>(*it, "scale", "major")).value_or(ScaleType::Major);
    }
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        auto& s = out.settings;
        const auto& sj = *it;
        s.metronome = get<bool>(sj, "metronome", s.metronome);
        s.metronomeGainDb = get<float>(sj, "metronomeGainDb", s.metronomeGainDb);
        s.countInBars = get<int>(sj, "countInBars", s.countInBars);
        s.preRollBeats = get<double>(sj, "preRollBeats", s.preRollBeats);
        s.punchEnabled = get<bool>(sj, "punchEnabled", s.punchEnabled);
        s.punchInBeat = get<double>(sj, "punchInBeat", s.punchInBeat);
        s.punchOutBeat = get<double>(sj, "punchOutBeat", s.punchOutBeat);
        s.snapBeats = get<double>(sj, "snapBeats", s.snapBeats);
        s.wrongNoteMode = wrongNoteModeFromId(get<std::string>(sj, "wrongNoteMode", "off")).value_or(WrongNoteMode::Off);
        s.autosaveIntervalSec = get<int>(sj, "autosaveIntervalSec", s.autosaveIntervalSec);
        s.backupRetention = get<int>(sj, "backupRetention", s.backupRetention);
        s.neverLoseSeconds = get<double>(sj, "neverLoseSeconds", s.neverLoseSeconds);
    }
    if (auto it = j.find("loop"); it != j.end() && it->is_object())
        out.loop = {get<bool>(*it, "enabled", false), get<double>(*it, "startBeat", 0.0), get<double>(*it, "endBeat", 16.0)};
    for (auto& m : j.value("markers", json::array()))
        out.markers.push_back({get<std::string>(m, "id", files::newId()), get<double>(m, "beat", 0.0),
                               get<std::string>(m, "name", ""), get<uint32_t>(m, "color", 0xFFFFF0)});
    for (auto& s : j.value("sections", json::array()))
        out.sections.push_back({get<std::string>(s, "id", files::newId()), get<std::string>(s, "name", ""),
                                get<std::string>(s, "type", "other"), get<double>(s, "startBeat", 0.0),
                                get<double>(s, "endBeat", 0.0), get<uint32_t>(s, "color", 0xFF8C00)});
    for (auto& a : j.value("assets", json::array())) out.assets.push_back(assetFrom(a));
    for (auto& c : j.value("channels", json::array())) out.channels.push_back(channelFrom(c));
    for (auto& t : j.value("tracks", json::array())) out.tracks.push_back(trackFrom(t));
    for (auto& a : j.value("automation", json::array())) {
        AutomationLane lane;
        lane.id = get<std::string>(a, "id", files::newId());
        lane.channelId = get<std::string>(a, "channelId", "");
        lane.slotId = get<std::string>(a, "slotId", "");
        lane.paramId = get<std::string>(a, "paramId", "");
        lane.enabled = get<bool>(a, "enabled", true);
        for (auto& pt : a.value("points", json::array())) {
            if (!pt.is_array() || pt.size() < 2 || !pt[0].is_number() || !pt[1].is_number()) continue;
            AutomationPoint ap{pt[0].get<double>(), pt[1].get<float>()};
            if (pt.size() >= 4 && pt[2].is_number_integer() && pt[3].is_number()) {
                ap.curve = std::clamp(pt[2].get<int>(), 0, 3);
                ap.tension = std::clamp(pt[3].get<float>(), -1.0f, 1.0f);
            }
            if (std::isfinite(ap.beat) && std::isfinite(ap.value)) lane.points.push_back(ap);
        }
        out.automation.push_back(lane);
    }
    for (auto& pat : j.value("patterns", json::array())) out.patterns.push_back(patternFrom(pat));
    out.presets = j.value("presets", json::object());
    out.producerMemory = j.value("producerMemory", json::object());
    out.vocalSettings = j.value("vocalSettings", json::object());
    out.metadata = j.value("metadata", json::object());
    for (auto& [k, v] : j.items())
        if (!knownTopLevelKeys().count(k)) out.unknown[k] = v;

    // Structural repair: a project always needs exactly one master.
    if (!out.master()) {
        MixerChannel m;
        m.id = files::newId();
        m.name = "MASTER";
        m.kind = ChannelKind::Master;
        out.channels.push_back(m);
        if (migrationLog) migrationLog->push_back("repair: added missing master channel");
    }
    for (auto& t : out.tracks)
        if (!out.findChannel(t.channelId)) {
            MixerChannel c;
            c.id = t.channelId.empty() ? files::newId() : t.channelId;
            c.name = t.name;
            t.channelId = c.id;
            out.channels.push_back(c);
            if (migrationLog) migrationLog->push_back("repair: added missing channel for track " + t.name);
        }
    p = std::move(out);
    return true;
}

std::string serializeProject(const Project& p, bool pretty) { return projectToJson(p).dump(pretty ? 2 : -1); }

bool deserializeProject(const std::string& text, Project& p, std::string* error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = "invalid JSON";
        return false;
    }
    return projectFromJson(j, p, error);
}

std::string sanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            static_cast<unsigned char>(c) < 32)
            out += '_';
        else
            out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "Untitled";
    return out;
}

fs::path projectFolderOf(const fs::path& projectFile) { return projectFile.parent_path(); }

fs::path createProjectFolder(const fs::path& parentDir, const std::string& name) {
    const std::string clean = sanitizeFileName(name);
    fs::path dir = files::uniquePath(parentDir / clean);
    std::error_code ec;
    for (const char* sub : {"Audio", "Exports", "BACKUPS", "RECOVERY", "Stems"}) fs::create_directories(dir / sub, ec);
    return dir / (dir.filename().string() + kProjectFileExtension);
}

std::vector<fs::path> listBackups(const fs::path& projectFile) {
    std::vector<fs::path> out;
    const fs::path dir = projectFolderOf(projectFile) / "BACKUPS";
    const std::string stem = projectFile.stem().string();
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    const std::regex re("^" + std::regex_replace(stem, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)") + R"(_(\d{4,})\.roy$)");
    for (auto& e : fs::directory_iterator(dir, ec)) {
        const auto fn = e.path().filename().string();
        if (std::regex_match(fn, re)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path writeBackup(const fs::path& projectFile, int retention, std::string* error) {
    std::error_code ec;
    if (!fs::exists(projectFile, ec)) return {};
    const fs::path dir = projectFolderOf(projectFile) / "BACKUPS";
    fs::create_directories(dir, ec);
    auto existing = listBackups(projectFile);
    int next = 1;
    if (!existing.empty()) {
        const auto fn = existing.back().stem().string();
        next = std::stoi(fn.substr(fn.rfind('_') + 1)) + 1;
    }
    const fs::path target = dir / std::format("{}_{:04d}.roy", projectFile.stem().string(), next);
    if (!files::safeCopy(projectFile, target, error)) return {};
    if (retention > 0) {
        existing = listBackups(projectFile);
        while (static_cast<int>(existing.size()) > retention) {
            fs::remove(existing.front(), ec);
            existing.erase(existing.begin());
        }
    }
    return target;
}

LoadResult loadProject(const fs::path& file, Project& p) {
    LoadResult r;
    auto text = files::readAll(file);
    if (!text) {
        r.error = "cannot read " + file.string();
        return r;
    }
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        r.error = "file is not a valid RoY project (JSON parse error)";
        return r;
    }
    r.fileVersion = j.value("formatVersion", 0);
    r.newerThanSupported = r.fileVersion > kProjectFormatVersion;
    if (r.fileVersion < kProjectFormatVersion) {
        // Keep an untouched copy of the original before migrating.
        const fs::path backup = files::uniquePath(projectFolderOf(file) / "BACKUPS" /
                                                  std::format("{}_pre_migration_v{}.roy", file.stem().string(), r.fileVersion));
        std::string err;
        if (!files::safeCopy(file, backup, &err)) {
            r.error = "cannot back up original before migration: " + err;
            return r;
        }
        r.migrationBackup = backup;
        r.migrated = true;
    }
    if (!projectFromJson(j, p, &r.error, &r.migrationLog)) return r;
    for (auto& m : r.migrationLog) log::info("project", "migration: {}", m);
    r.ok = true;
    return r;
}

SaveResult saveProject(const Project& pIn, const fs::path& file, const SaveOptions& opt) {
    SaveResult r;
    std::error_code ec;
    if (fs::exists(file, ec) && !opt.allowOverwriteNewer) {
        if (auto text = files::readAll(file)) {
            json old = json::parse(*text, nullptr, false);
            if (!old.is_discarded() && old.value("formatVersion", 0) > kProjectFormatVersion) {
                r.error = "the file on disk was written by a newer RoY Studio version; save as a new file instead";
                return r;
            }
        }
    }
    if (opt.makeBackup && fs::exists(file, ec)) {
        std::string err;
        r.backupPath = writeBackup(file, opt.backupRetention, &err);
        if (r.backupPath.empty()) {
            r.error = "backup failed, project NOT saved: " + err;
            return r;
        }
    }
    Project p = pIn;
    p.modifiedAt = files::nowIso8601();
    const std::string text = serializeProject(p);
    // Verify the serialized text parses back before touching the target.
    Project verify;
    std::string verr;
    if (!deserializeProject(text, verify, &verr)) {
        r.error = "internal serialization check failed: " + verr;
        return r;
    }
    if (!files::atomicWrite(file, text, &r.error)) return r;
    r.ok = true;
    log::info("project", "saved '{}' to {}", p.name, file.string());
    return r;
}

} // namespace roy
