#include "project/Project.h"
#include "core/Files.h"

#include <algorithm>
#include <set>

namespace roy {

Track* Project::findTrack(const std::string& id) {
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}
const Track* Project::findTrack(const std::string& id) const {
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}
MixerChannel* Project::findChannel(const std::string& id) {
    for (auto& c : channels) if (c.id == id) return &c;
    return nullptr;
}
const MixerChannel* Project::findChannel(const std::string& id) const {
    for (auto& c : channels) if (c.id == id) return &c;
    return nullptr;
}
MixerChannel* Project::master() {
    for (auto& c : channels) if (c.kind == ChannelKind::Master) return &c;
    return nullptr;
}
const MixerChannel* Project::master() const {
    for (auto& c : channels) if (c.kind == ChannelKind::Master) return &c;
    return nullptr;
}
AudioAsset* Project::findAsset(const std::string& id) {
    for (auto& a : assets) if (a.id == id) return &a;
    return nullptr;
}
const AudioAsset* Project::findAsset(const std::string& id) const {
    for (auto& a : assets) if (a.id == id) return &a;
    return nullptr;
}
Pattern* Project::findPattern(const std::string& id) {
    for (auto& p : patterns) if (p.id == id) return &p;
    return nullptr;
}
const Pattern* Project::findPattern(const std::string& id) const {
    for (auto& p : patterns) if (p.id == id) return &p;
    return nullptr;
}
AudioClip* Project::findAudioClip(const std::string& clipId, Track** owner) {
    for (auto& t : tracks)
        for (auto& c : t.audioClips)
            if (c.id == clipId) {
                if (owner) *owner = &t;
                return &c;
            }
    return nullptr;
}
MidiClip* Project::findMidiClip(const std::string& clipId, Track** owner) {
    for (auto& t : tracks)
        for (auto& c : t.midiClips)
            if (c.id == clipId) {
                if (owner) *owner = &t;
                return &c;
            }
    return nullptr;
}
PluginSlot* Project::findSlot(const std::string& slotId, MixerChannel** owner) {
    for (auto& ch : channels)
        for (auto& s : ch.inserts)
            if (s.id == slotId) {
                if (owner) *owner = &ch;
                return &s;
            }
    for (auto& t : tracks)
        if (t.instrument && t.instrument->id == slotId) {
            if (owner) *owner = findChannel(t.channelId);
            return &*t.instrument;
        }
    return nullptr;
}

double Project::endBeat() const {
    double end = 0.0;
    for (auto& t : tracks) {
        for (auto& c : t.audioClips) end = std::max(end, c.endBeat());
        for (auto& c : t.midiClips) end = std::max(end, c.endBeat());
        for (auto& c : t.patternClips) end = std::max(end, c.endBeat());
    }
    for (auto& s : sections) end = std::max(end, s.endBeat);
    return end;
}

Project makeNewProject(const std::string& name, double sampleRate, double bpm) {
    Project p;
    p.id = files::newId();
    p.name = name;
    p.createdAt = p.modifiedAt = files::nowIso8601();
#ifdef ROY_VERSION_STRING
    p.appVersion = ROY_VERSION_STRING;
#endif
    p.sampleRate = sampleRate;
    p.tempo = TempoMap(bpm, 4, 4);
    MixerChannel master;
    master.id = files::newId();
    master.name = "MASTER";
    master.kind = ChannelKind::Master;
    p.channels.push_back(master);
    for (const char* bus : {"VOCALS", "DRUMS", "MUSIC", "FX"}) addBus(p, bus);
    return p;
}

MixerChannel& addBus(Project& p, const std::string& name) {
    MixerChannel c;
    c.id = files::newId();
    c.name = name;
    c.kind = ChannelKind::Bus;
    c.color = 0xFF8C00;
    p.channels.push_back(c);
    return p.channels.back();
}

Track& addTrack(Project& p, TrackType type, const std::string& name, const std::string& outputChannelId) {
    MixerChannel ch;
    ch.id = files::newId();
    ch.name = name;
    ch.kind = ChannelKind::Track;
    ch.outputChannelId = outputChannelId;
    p.channels.push_back(ch);

    Track t;
    t.id = files::newId();
    t.name = name;
    t.type = type;
    t.channelId = ch.id;
    if (type == TrackType::Midi) {
        PluginSlot inst;
        inst.id = files::newId();
        inst.typeId = "roy.synth";
        inst.name = "RoY Synth";
        t.instrument = inst;
        t.color = 0xFF8C00;
    } else if (type == TrackType::Beat) {
        PluginSlot inst;
        inst.id = files::newId();
        inst.typeId = "roy.drums";
        inst.name = "RoY Drums";
        t.instrument = inst;
        t.color = 0x2E8B57;
    }
    p.tracks.push_back(std::move(t));
    return p.tracks.back();
}

namespace {
// Removes a mixer channel and every reference to it: its automation, sends into it
// (and their automation lanes), sidechain keys. Channels routed into it are re-routed
// to where it was going, so no audio silently disappears.
void removeChannelRefs(Project& p, const std::string& chId) {
    auto ch = std::find_if(p.channels.begin(), p.channels.end(), [&](auto& c) { return c.id == chId; });
    if (ch == p.channels.end()) return;
    const std::string downstream = ch->outputChannelId;
    std::vector<std::string> ownSends;
    for (auto& s : ch->sends) ownSends.push_back(s.id);
    p.channels.erase(ch);
    std::vector<std::string> deadSends = ownSends;
    for (auto& c : p.channels) {
        for (auto& s : c.sends)
            if (s.targetChannelId == chId) deadSends.push_back(s.id);
        std::erase_if(c.sends, [&](auto& s) { return s.targetChannelId == chId; });
        for (auto& ins : c.inserts)
            if (ins.sidechainChannelId == chId) ins.sidechainChannelId.clear();
        if (c.outputChannelId == chId) c.outputChannelId = downstream;
    }
    std::erase_if(p.automation, [&](auto& a) {
        if (a.channelId == chId) return true;
        for (auto& id : deadSends)
            if (a.paramId == "send:" + id) return true;
        return false;
    });
}
} // namespace

bool removeTrack(Project& p, const std::string& trackId) {
    auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [&](auto& t) { return t.id == trackId; });
    if (it == p.tracks.end()) return false;
    const std::string chId = it->channelId;
    p.tracks.erase(it);
    removeChannelRefs(p, chId);
    return true;
}

bool routingWouldLoop(const Project& p, const std::string& from, const std::string& to) {
    if (from == to) return true;
    const MixerChannel* master = p.master();
    // signal edges: output (empty = master), sends, sidechain source -> consumer
    auto next = [&](const MixerChannel& c) {
        std::vector<std::string> n;
        if (c.kind != ChannelKind::Master) n.push_back(!c.outputChannelId.empty() ? c.outputChannelId : master ? master->id : "");
        for (auto& s : c.sends) n.push_back(s.targetChannelId);
        for (auto& other : p.channels)
            for (auto& ins : other.inserts)
                if (ins.sidechainChannelId == c.id) n.push_back(other.id);
        return n;
    };
    std::vector<std::string> stack{to};
    std::set<std::string> seen;
    while (!stack.empty()) {
        const std::string id = stack.back();
        stack.pop_back();
        if (id == from) return true;
        if (id.empty() || !seen.insert(id).second) continue;
        if (const MixerChannel* c = p.findChannel(id))
            for (auto& n : next(*c)) stack.push_back(n);
    }
    return false;
}

bool removeBus(Project& p, const std::string& channelId) {
    const MixerChannel* c = p.findChannel(channelId);
    if (!c || c->kind != ChannelKind::Bus) return false;
    removeChannelRefs(p, channelId);
    return true;
}

Pattern makeDefaultPattern(const std::string& name, int numSteps) {
    Pattern pat;
    pat.id = files::newId();
    pat.name = name;
    pat.numSteps = numSteps;
    struct RowDef { const char* name; const char* voice; int note; };
    static const RowDef rows[] = {{"Kick", "kick", 36},       {"Snare", "snare", 38},  {"Clap", "clap", 39},
                                  {"Closed Hat", "closed_hat", 42}, {"Open Hat", "open_hat", 46}, {"Percussion", "perc", 47},
                                  {"Rim", "rim", 37},         {"808", "808", 35},      {"FX", "fx", 49}};
    for (auto& r : rows) {
        PatternRow row;
        row.id = files::newId();
        row.name = r.name;
        row.voice = r.voice;
        row.note = r.note;
        row.steps.resize(static_cast<size_t>(numSteps));
        pat.rows.push_back(row);
    }
    return pat;
}

} // namespace roy
