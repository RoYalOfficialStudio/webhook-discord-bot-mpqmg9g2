#include "midi/MidiLearn.h"
#include "audio/ProjectRuntime.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace roy::midi {

namespace {
const Send* findSend(const MixerChannel& ch, const std::string& paramId, int* index = nullptr) {
    if (paramId.rfind("send:", 0) != 0) return nullptr;
    const std::string id = paramId.substr(5);
    for (size_t i = 0; i < ch.sends.size(); ++i)
        if (ch.sends[i].id == id) {
            if (index) *index = static_cast<int>(i);
            return &ch.sends[i];
        }
    return nullptr;
}

const ParamInfo* processorParam(ProjectRuntime* rt, const std::string& slotId, const std::string& paramId, std::shared_ptr<Processor>* out = nullptr) {
    if (!rt) return nullptr;
    auto proc = rt->processorForSlot(slotId);
    if (!proc) return nullptr;
    const int i = proc->findParam(paramId);
    if (i < 0) return nullptr;
    if (out) *out = proc;
    return &proc->paramInfo(i);
}
} // namespace

bool resolveTarget(Project& p, ProjectRuntime* rt, MidiTarget& t, float* lo, float* hi, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    float a = 0, b = 1;
    if (!t.slotId.empty()) {
        MixerChannel* owner = nullptr;
        PluginSlot* slot = p.findSlot(t.slotId, &owner);
        if (!slot || !owner) return fail("effect/instrument not found");
        t.channelId = owner->id;
        if (const ParamInfo* pi = processorParam(rt, t.slotId, t.paramId)) {
            a = pi->minValue;
            b = pi->maxValue;
        } else if (rt && rt->processorForSlot(t.slotId)) {
            return fail("unknown parameter " + t.paramId);
        } else if (!(slot->state.is_object() && slot->state.contains("params") && slot->state["params"].contains(t.paramId))) {
            return fail("parameter " + t.paramId + " unknown (plugin not loaded)");
        }
    } else {
        const MixerChannel* ch = p.findChannel(t.channelId);
        if (!ch) return fail("channel not found");
        if (t.paramId == "gain") a = -60.0f, b = 6.0f;
        else if (t.paramId == "pan") a = -1.0f, b = 1.0f;
        else if (t.paramId == "width") a = 0.0f, b = 2.0f;
        else if (findSend(*ch, t.paramId)) a = -60.0f, b = 6.0f;
        else return fail("unknown channel parameter " + t.paramId);
    }
    if (lo) *lo = a;
    if (hi) *hi = b;
    return true;
}

bool mappingTargetExists(Project& p, ProjectRuntime* rt, const MidiMapping& m) {
    MidiTarget t{m.channelId, m.slotId, m.paramId};
    return resolveTarget(p, rt, t);
}

std::string targetName(Project& p, ProjectRuntime* rt, const MidiMapping& m) {
    const MixerChannel* ch = p.findChannel(m.channelId);
    std::string s = ch ? ch->name : "?";
    if (!m.slotId.empty()) {
        const PluginSlot* slot = p.findSlot(m.slotId);
        s += " · " + (slot ? slot->name : std::string("?"));
        const ParamInfo* pi = processorParam(rt, m.slotId, m.paramId);
        return s + " · " + (pi ? pi->name : m.paramId);
    }
    if (m.paramId == "gain") return s + " · Volume";
    if (m.paramId == "pan") return s + " · Pan";
    if (m.paramId == "width") return s + " · Width";
    if (ch)
        if (const Send* snd = findSend(*ch, m.paramId)) {
            const MixerChannel* to = p.findChannel(snd->targetChannelId);
            return s + " · Send " + (to ? to->name : "?");
        }
    return s + " · " + m.paramId;
}

const MidiMapping* findMappingForTarget(const Project& p, const std::string& channelId, const std::string& slotId, const std::string& paramId) {
    for (auto& m : p.midiMappings)
        if (m.paramId == paramId && m.slotId == slotId && (!slotId.empty() || m.channelId == channelId)) return &m;
    return nullptr;
}

float mappedValue(const MidiMapping& m, int value7, int steps) {
    const float x = static_cast<float>(std::clamp(value7, 0, 127)) / 127.0f;
    float v = m.minValue + (m.maxValue - m.minValue) * x;
    if (steps == 2) v = x >= 0.5f ? std::max(m.minValue, m.maxValue) : std::min(m.minValue, m.maxValue);
    else if (steps > 2) v = std::round(v);
    return v;
}

int applyControls(Project& p, ProjectRuntime* rt, const std::vector<ControlChange>& ccs) {
    if (p.midiMappings.empty() || ccs.empty()) return 0;
    std::map<size_t, int> last; // mapping index -> newest CC value
    for (auto& c : ccs)
        for (size_t i = 0; i < p.midiMappings.size(); ++i) {
            const auto& m = p.midiMappings[i];
            if (m.cc == c.cc && (m.channel < 0 || m.channel == c.channel)) last[i] = c.value;
        }
    int changed = 0;
    for (auto& [i, value] : last) {
        const MidiMapping m = p.midiMappings[i];
        if (!m.slotId.empty()) {
            PluginSlot* slot = p.findSlot(m.slotId);
            if (!slot) continue;
            std::shared_ptr<Processor> proc;
            const ParamInfo* pi = processorParam(rt, m.slotId, m.paramId, &proc);
            const float v = mappedValue(m, value, pi ? pi->steps : 0);
            if (!slot->state.is_object()) slot->state = json::object();
            slot->state["params"][m.paramId] = v;
            if (proc) proc->setParam(m.paramId, v);
            ++changed;
            continue;
        }
        MixerChannel* ch = p.findChannel(m.channelId);
        if (!ch) continue;
        const float v = mappedValue(m, value);
        auto params = rt ? rt->channelParams(ch->id) : nullptr;
        int sendIndex = -1;
        if (m.paramId == "gain") {
            ch->gainDb = std::clamp(v, -120.0f, 12.0f);
            if (params) params->gainDb.store(ch->gainDb);
        } else if (m.paramId == "pan") {
            ch->pan = std::clamp(v, -1.0f, 1.0f);
            if (params) params->pan.store(ch->pan);
        } else if (m.paramId == "width") {
            ch->width = std::clamp(v, 0.0f, 2.0f);
            if (params) params->width.store(ch->width);
        } else if (findSend(*ch, m.paramId, &sendIndex)) {
            Send& s = ch->sends[static_cast<size_t>(sendIndex)];
            s.levelDb = std::clamp(v, -120.0f, 12.0f);
            if (params && sendIndex < ChannelParams::kMaxSends) params->sendLevelDb[sendIndex].store(s.levelDb);
        } else {
            continue;
        }
        ++changed;
    }
    return changed;
}

} // namespace roy::midi
