#include "commands/Commands.h"
#include "project/Automation.h"
#include "project/AutomationShape.h"
#include "arrange/ClipOps.h"
#include "midi/MidiFile.h"
#include "midi/MidiOps.h"
#include "record/Takes.h"
#include "audio/ProjectRuntime.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Math.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace roy {

// ---------------------------------------------------------------- registry
void CommandRegistry::add(CommandInfo info) { commands_[info.id] = std::move(info); }

const CommandInfo* CommandRegistry::find(const std::string& id) const {
    auto it = commands_.find(id);
    return it == commands_.end() ? nullptr : &it->second;
}

std::vector<const CommandInfo*> CommandRegistry::list(const std::string& category) const {
    std::vector<const CommandInfo*> out;
    for (auto& [id, c] : commands_)
        if (category.empty() || c.category == category) out.push_back(&c);
    return out;
}

std::vector<const CommandInfo*> CommandRegistry::search(const std::string& query) const {
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string q = lower(query);
    std::vector<std::pair<int, const CommandInfo*>> scored;
    for (auto& [id, c] : commands_) {
        const std::string hay = lower(c.id + " " + c.title + " " + c.category);
        size_t pos = 0;
        int score = 0;
        bool ok = true;
        for (char ch : q) {
            if (ch == ' ') continue;
            const size_t f = hay.find(ch, pos);
            if (f == std::string::npos) { ok = false; break; }
            score += static_cast<int>(f - pos);
            pos = f + 1;
        }
        if (ok) scored.push_back({score, &c});
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<const CommandInfo*> out;
    for (auto& s : scored) out.push_back(s.second);
    return out;
}

const CommandInfo* CommandRegistry::byShortcut(const std::string& shortcut) const {
    for (auto& [id, c] : commands_)
        if (!c.shortcut.empty() && c.shortcut == shortcut) return &c;
    return nullptr;
}

bool CommandRegistry::setShortcut(const std::string& id, const std::string& shortcut) {
    auto it = commands_.find(id);
    if (it == commands_.end()) return false;
    for (auto& [k, c] : commands_)
        if (c.shortcut == shortcut) c.shortcut.clear(); // a shortcut maps to one command
    it->second.shortcut = shortcut;
    return true;
}

bool CommandRegistry::execute(CommandContext& ctx, const std::string& id, const json& args) const {
    const CommandInfo* c = find(id);
    if (!c) {
        ctx.error = "unknown command: " + id;
        return false;
    }
    ctx.error.clear();
    ctx.result = json::object();
    if (c->undoable) ctx.undo.begin(c->title);
    bool ok = false;
    try {
        ok = c->fn(ctx, args);
    } catch (const std::exception& e) {
        ctx.error = std::string("command failed: ") + e.what();
        ok = false;
    }
    if (c->undoable) {
        if (ok) ctx.undo.end();
        else ctx.undo.cancel();
    }
    if (ok && ctx.changed) ctx.changed(c->structural);
    if (!ok) log::warn("command", "{} failed: {}", id, ctx.error);
    return ok;
}

bool CommandRegistry::executeMacro(CommandContext& ctx, const std::string& name,
                                   const std::vector<std::pair<std::string, json>>& steps) const {
    ctx.undo.begin(name);
    bool structural = false;
    for (auto& [id, args] : steps) {
        const CommandInfo* c = find(id);
        if (!c) {
            ctx.error = "unknown command in macro: " + id;
            ctx.undo.cancel();
            return false;
        }
        bool ok = false;
        try {
            ok = c->fn(ctx, args);
        } catch (const std::exception& e) {
            ctx.error = e.what();
        }
        if (!ok) {
            ctx.error = "macro step " + id + " failed: " + ctx.error;
            ctx.undo.cancel();
            return false;
        }
        structural |= c->structural;
    }
    ctx.undo.end();
    if (ctx.changed) ctx.changed(structural);
    return true;
}

// ---------------------------------------------------------------- helpers
namespace {

std::string argStr(const json& a, const char* k, const std::string& def = {}) {
    auto it = a.find(k);
    return (it != a.end() && it->is_string()) ? it->get<std::string>() : def;
}
double argNum(const json& a, const char* k, double def) {
    auto it = a.find(k);
    return (it != a.end() && it->is_number()) ? it->get<double>() : def;
}
bool argBool(const json& a, const char* k, bool def) {
    auto it = a.find(k);
    return (it != a.end() && it->is_boolean()) ? it->get<bool>() : def;
}

bool fail(CommandContext& ctx, const std::string& e) {
    ctx.error = e;
    return false;
}

bool fromEdit(CommandContext& ctx, const arrange::EditResult& r) {
    if (!r.ok) return fail(ctx, r.error);
    if (!r.newId.empty()) ctx.result["id"] = r.newId;
    return true;
}

MixerChannel* channelArg(CommandContext& ctx, const json& a) {
    if (argBool(a, "master", false)) return ctx.project.master();
    std::string id = argStr(a, "channelId");
    if (id.empty()) {
        if (auto* t = ctx.project.findTrack(argStr(a, "trackId"))) id = t->channelId;
    }
    return ctx.project.findChannel(id);
}

TrackType trackTypeArg(const std::string& s) {
    if (s == "midi") return TrackType::Midi;
    if (s == "beat") return TrackType::Beat;
    return TrackType::Audio;
}

} // namespace

// ---------------------------------------------------------------- core commands
void registerCoreCommands(CommandRegistry& r) {
    // ---- tracks ---------------------------------------------------------------
    r.add({"AddTrack", "Add Track", "Track", "Ctrl+T", true, true, [](CommandContext& ctx, const json& a) {
               const auto type = trackTypeArg(argStr(a, "type", "audio"));
               std::string out = argStr(a, "output");
               if (!out.empty() && !ctx.project.findChannel(out)) return fail(ctx, "output channel not found");
               auto& t = addTrack(ctx.project, type, argStr(a, "name", "Track"), out);
               t.role = argStr(a, "role");
               if (a.contains("instrument") && t.instrument) {
                   const std::string type = argStr(a, "instrument");
                   if (!ProcessorFactory::instance().has(type)) return fail(ctx, "unknown instrument " + type);
                   t.instrument->typeId = type;
                   t.instrument->name = type;
                   for (auto& e : ProcessorFactory::instance().entries())
                       if (e.typeId == type) t.instrument->name = e.displayName;
               }
               ctx.result["id"] = t.id;
               ctx.result["channelId"] = t.channelId;
               return true;
           }});
    r.add({"DeleteTrack", "Delete Track", "Track", "", true, true, [](CommandContext& ctx, const json& a) {
               const Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               return removeTrack(ctx.project, t->id) || fail(ctx, "could not remove track");
           }});
    r.add({"RenameTrack", "Rename Track", "Track", "F2", true, false, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               t->name = argStr(a, "name", t->name);
               if (auto* ch = ctx.project.findChannel(t->channelId)) ch->name = t->name;
               return true;
           }});
    r.add({"ArmTrack", "Arm Track", "Track", "", true, false, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               t->armed = argBool(a, "armed", !t->armed);
               return true;
           }});
    r.add({"MonitorTrack", "Toggle Input Monitoring", "Track", "", true, false, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               t->monitor = argBool(a, "monitor", !t->monitor);
               return true;
           }});
    r.add({"SetTrackInput", "Set Track Input", "Track", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               t->inputLeft = static_cast<int>(argNum(a, "left", t->inputLeft));
               t->inputRight = static_cast<int>(argNum(a, "right", t->inputRight));
               t->inputGainDb = static_cast<float>(argNum(a, "gainDb", t->inputGainDb));
               return true;
           }});
    r.add({"LockTrack", "Lock Track", "Track", "", true, false, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               t->locked = argBool(a, "locked", !t->locked);
               return true;
           }});

    // ---- take lanes + comping -------------------------------------------------
    // Every recorded pass is a Take on its own lane; the comp says which take plays where.
    // Deleting a take removes it from the model only - its audio file stays in the project.
    r.add({"CompSelect", "Comp Range From Take", "Take", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               const double s = argNum(a, "startBeat", 0.0), e = argNum(a, "endBeat", 0.0);
               if (!(e > s)) return fail(ctx, "empty comp range");
               return takes::compSelect(*t, argStr(a, "takeId"), s, e) || fail(ctx, "take not found or range outside the take");
           }});
    r.add({"CompWholeTake", "Use Whole Take", "Take", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               return takes::compWholeTake(*t, argStr(a, "takeId")) || fail(ctx, "take not found");
           }});
    r.add({"ClearComp", "Clear Comp", "Take", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               t->comp.clear();
               return true;
           }});
    r.add({"FlattenComp", "Flatten Comp To Clips", "Take", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               if (t->comp.empty()) return fail(ctx, "track has no comp");
               const auto ids = takes::flattenComp(ctx.project, t->id);
               ctx.result["clipIds"] = ids;
               return !ids.empty() || fail(ctx, "comp references no existing take");
           }});
    r.add({"DeleteTake", "Delete Take", "Take", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->locked) return fail(ctx, "track is locked");
               return takes::removeTake(*t, argStr(a, "takeId")) || fail(ctx, "take not found");
           }});
    r.add({"RenameTake", "Rename Take", "Take", "", true, false, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               for (auto& k : t->takes)
                   if (k.id == argStr(a, "takeId")) {
                       const std::string n = argStr(a, "name");
                       if (n.empty()) return fail(ctx, "empty name");
                       k.name = n;
                       return true;
                   }
               return fail(ctx, "take not found");
           }});

    // ---- mixer ---------------------------------------------------------------
    r.add({"SetChannelGain", "Set Volume", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               const double v = argNum(a, "gainDb", 0.0);
               const double target = argBool(a, "relative", false) ? ch->gainDb + v : argNum(a, "gainDb", ch->gainDb);
               ch->gainDb = static_cast<float>(std::clamp(target, -120.0, 12.0));
               return true;
           }});
    r.add({"SetChannelPan", "Set Pan", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               ch->pan = static_cast<float>(std::clamp(argNum(a, "pan", ch->pan), -1.0, 1.0));
               return true;
           }});
    r.add({"SetChannelWidth", "Set Stereo Width", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               ch->width = static_cast<float>(std::clamp(argNum(a, "width", ch->width), 0.0, 2.0));
               return true;
           }});
    r.add({"MuteChannel", "Mute", "Mixer", "M", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               ch->mute = argBool(a, "mute", !ch->mute);
               return true;
           }});
    r.add({"SoloChannel", "Solo", "Mixer", "S", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               ch->solo = argBool(a, "solo", !ch->solo);
               return true;
           }});
    r.add({"SetSoloSafe", "Solo Safe", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               if (ch->kind == ChannelKind::Master) return fail(ctx, "the master is never muted by solo");
               ch->soloSafe = argBool(a, "safe", !ch->soloSafe);
               return true;
           }});
    r.add({"InvertPhase", "Invert Phase", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               ch->phaseInvert = argBool(a, "invert", !ch->phaseInvert);
               return true;
           }});
    r.add({"AddBus", "Add Bus", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               auto& b = addBus(ctx.project, argStr(a, "name", "BUS"));
               ctx.result["id"] = b.id;
               return true;
           }});
    r.add({"DeleteBus", "Delete Bus", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               return removeBus(ctx.project, argStr(a, "channelId")) || fail(ctx, "bus not found");
           }});
    r.add({"RenameChannel", "Rename Channel", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               const std::string name = argStr(a, "name");
               if (name.empty()) return fail(ctx, "name must not be empty");
               ch->name = name;
               for (auto& t : ctx.project.tracks)
                   if (t.channelId == ch->id) t.name = name;
               return true;
           }});
    r.add({"RouteChannel", "Route Output", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               if (ch->kind == ChannelKind::Master) return fail(ctx, "master has no output routing");
               const std::string out = argStr(a, "output");
               if (!out.empty()) {
                   const auto* target = ctx.project.findChannel(out);
                   if (!target) return fail(ctx, "output channel not found");
                   if (target->kind == ChannelKind::Track) return fail(ctx, "channels can only route to busses or master");
                   if (out == ch->id) return fail(ctx, "cannot route a channel into itself");
                   if (routingWouldLoop(ctx.project, ch->id, out)) return fail(ctx, "routing would create a feedback loop");
               }
               ch->outputChannelId = out;
               return true;
           }});
    r.add({"AddSend", "Add Send", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               const std::string target = argStr(a, "target");
               const auto* t = ctx.project.findChannel(target);
               if (!t || t->kind != ChannelKind::Bus) return fail(ctx, "send target must be a bus");
               if (target == ch->id) return fail(ctx, "cannot send to itself");
               if (ch->sends.size() >= 16) return fail(ctx, "maximum 16 sends per channel");
               if (routingWouldLoop(ctx.project, ch->id, target)) return fail(ctx, "this send would create a feedback loop");
               Send s;
               s.id = files::newId();
               s.targetChannelId = target;
               s.levelDb = static_cast<float>(argNum(a, "levelDb", -6.0));
               s.preFader = argBool(a, "preFader", false);
               ch->sends.push_back(s);
               ctx.result["id"] = s.id;
               return true;
           }});
    r.add({"RemoveSend", "Remove Send", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               const std::string id = argStr(a, "sendId");
               for (auto& ch : ctx.project.channels)
                   if (std::erase_if(ch.sends, [&](const Send& s) { return s.id == id; }) > 0) {
                       std::erase_if(ctx.project.automation, [&](const AutomationLane& l) { return l.paramId == "send:" + id; });
                       return true;
                   }
               return fail(ctx, "send not found");
           }});
    r.add({"SetSend", "Set Send Level", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               for (auto& ch : ctx.project.channels)
                   for (auto& s : ch.sends)
                       if (s.id == argStr(a, "sendId")) {
                           s.levelDb = static_cast<float>(argNum(a, "levelDb", s.levelDb));
                           s.enabled = argBool(a, "enabled", s.enabled);
                           if (a.contains("preFader") && argBool(a, "preFader", s.preFader) != s.preFader) {
                               s.preFader = argBool(a, "preFader", s.preFader);
                           }
                           return true;
                       }
               return fail(ctx, "send not found");
           }});
    r.add({"AddInsert", "Add Effect", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               const std::string type = argStr(a, "typeId");
               if (!ProcessorFactory::instance().has(type)) return fail(ctx, "unknown effect type " + type);
               if (ch->inserts.size() >= 32) return fail(ctx, "maximum 32 inserts per channel");
               PluginSlot s;
               s.id = files::newId();
               s.typeId = type;
               s.name = argStr(a, "name", type);
               if (a.contains("params") && a["params"].is_object()) s.state["params"] = a["params"];
               s.sidechainChannelId = argStr(a, "sidechain");
               if (!s.sidechainChannelId.empty()) {
                   if (!ctx.project.findChannel(s.sidechainChannelId)) return fail(ctx, "sidechain source not found");
                   if (routingWouldLoop(ctx.project, s.sidechainChannelId, ch->id)) return fail(ctx, "this sidechain would create a feedback loop");
               }
               const int index = static_cast<int>(argNum(a, "index", static_cast<double>(ch->inserts.size())));
               ch->inserts.insert(ch->inserts.begin() + std::clamp(index, 0, static_cast<int>(ch->inserts.size())), s);
               ctx.result["id"] = s.id;
               return true;
           }});
    r.add({"SetSidechain", "Set Sidechain Source", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               MixerChannel* owner = nullptr;
               PluginSlot* slot = ctx.project.findSlot(argStr(a, "slotId"), &owner);
               if (!slot || !owner) return fail(ctx, "effect not found");
               const std::string src = argStr(a, "sidechain");
               if (!src.empty()) {
                   if (!ctx.project.findChannel(src)) return fail(ctx, "sidechain source not found");
                   const std::string old = slot->sidechainChannelId;
                   slot->sidechainChannelId.clear(); // judge the new edge without the old one
                   const bool loop = routingWouldLoop(ctx.project, src, owner->id);
                   slot->sidechainChannelId = old;
                   if (loop) return fail(ctx, "this sidechain would create a feedback loop");
               }
               slot->sidechainChannelId = src;
               return true;
           }});
    r.add({"RemoveInsert", "Remove Effect", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               const std::string id = argStr(a, "slotId");
               for (auto& ch : ctx.project.channels)
                   if (std::erase_if(ch.inserts, [&](auto& s) { return s.id == id; }) > 0) {
                       std::erase_if(ctx.project.automation, [&](auto& l) { return l.slotId == id; });
                       return true;
                   }
               return fail(ctx, "effect not found");
           }});
    r.add({"MoveInsert", "Reorder Effect", "Mixer", "", true, true, [](CommandContext& ctx, const json& a) {
               MixerChannel* owner = nullptr;
               const std::string id = argStr(a, "slotId");
               if (!ctx.project.findSlot(id, &owner) || !owner) return fail(ctx, "effect not found");
               auto it = std::find_if(owner->inserts.begin(), owner->inserts.end(), [&](auto& s) { return s.id == id; });
               if (it == owner->inserts.end()) return fail(ctx, "instrument slots cannot be reordered");
               PluginSlot s = *it;
               owner->inserts.erase(it);
               const int idx = std::clamp(static_cast<int>(argNum(a, "index", 0)), 0, static_cast<int>(owner->inserts.size()));
               owner->inserts.insert(owner->inserts.begin() + idx, s);
               return true;
           }});
    r.add({"BypassInsert", "Bypass Effect", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               PluginSlot* s = ctx.project.findSlot(argStr(a, "slotId"));
               if (!s) return fail(ctx, "effect not found");
               s->bypass = argBool(a, "bypass", !s->bypass);
               return true;
           }});
    r.add({"SetParam", "Set Parameter", "Mixer", "", true, false, [](CommandContext& ctx, const json& a) {
               PluginSlot* s = ctx.project.findSlot(argStr(a, "slotId"));
               if (!s) return fail(ctx, "slot not found");
               const std::string pid = argStr(a, "paramId");
               const double v = argNum(a, "value", 0.0);
               if (!s->state.is_object()) s->state = json::object();
               s->state["params"][pid] = v;
               if (ctx.runtime)
                   if (auto proc = ctx.runtime->processorForSlot(s->id))
                       if (!proc->setParam(pid, static_cast<float>(v))) return fail(ctx, "unknown parameter " + pid);
               return true;
           }});
    r.add({"SetInstrument", "Set Instrument", "Track", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t) return fail(ctx, "track not found");
               if (t->type == TrackType::Audio) return fail(ctx, "audio tracks have no instrument");
               const std::string type = argStr(a, "typeId");
               if (!ProcessorFactory::instance().has(type)) return fail(ctx, "unknown instrument " + type);
               PluginSlot s;
               s.id = files::newId();
               s.typeId = type;
               s.name = argStr(a, "name", type);
               t->instrument = s;
               ctx.result["id"] = s.id;
               return true;
           }});

    // ---- clips ---------------------------------------------------------------
    r.add({"AddAudioClip", "Add Audio Clip", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t || t->type != TrackType::Audio) return fail(ctx, "audio track not found");
               const AudioAsset* as = ctx.project.findAsset(argStr(a, "assetId"));
               if (!as) return fail(ctx, "asset not found");
               AudioClip c;
               c.id = files::newId();
               c.assetId = as->id;
               c.name = argStr(a, "name", as->originalName);
               c.startBeat = std::max(0.0, argNum(a, "startBeat", 0.0));
               const double srcSec = as->sampleRate > 0 ? static_cast<double>(as->frames) / as->sampleRate : 1.0;
               const double defLen = ctx.project.tempo.secondsToBeat(ctx.project.tempo.beatToSeconds(c.startBeat) + srcSec) - c.startBeat;
               c.lengthBeats = argNum(a, "lengthBeats", defLen);
               t->audioClips.push_back(c);
               ctx.result["id"] = c.id;
               return true;
           }});
    r.add({"MoveClip", "Move Clip", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::moveClip(ctx.project, argStr(a, "clipId"), argNum(a, "startBeat", 0.0),
                                                      argStr(a, "trackId"), argBool(a, "moveGroup", true)));
           }});
    // Moves several clips by the same offset as ONE undo step (all or nothing).
    r.add({"MoveClips", "Move Clips", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               const auto ids = a.value("clipIds", json::array());
               if (!ids.is_array() || ids.empty()) return fail(ctx, "clipIds required");
               const double delta = argNum(a, "deltaBeats", 0.0);
               std::vector<std::pair<std::string, double>> moves;
               for (auto& id : ids) {
                   if (!id.is_string()) return fail(ctx, "clip ids must be strings");
                   const std::string cid = id.get<std::string>();
                   std::optional<double> start;
                   for (auto& t : ctx.project.tracks) {
                       for (auto& c : t.audioClips)
                           if (c.id == cid) start = c.startBeat;
                       for (auto& c : t.midiClips)
                           if (c.id == cid) start = c.startBeat;
                       for (auto& c : t.patternClips)
                           if (c.id == cid) start = c.startBeat;
                   }
                   if (!start) return fail(ctx, "clip not found: " + cid);
                   if (*start + delta < 0) return fail(ctx, "clips cannot move before the song start");
                   moves.emplace_back(cid, *start + delta);
               }
               for (auto& [cid, to] : moves)
                   if (!fromEdit(ctx, arrange::moveClip(ctx.project, cid, to, {}, false))) return false;
               return true;
           }});
    r.add({"DuplicateClip", "Duplicate Clip", "Clip", "Ctrl+D", true, true, [](CommandContext& ctx, const json& a) {
               std::optional<double> at;
               if (a.contains("startBeat")) at = argNum(a, "startBeat", 0.0);
               return fromEdit(ctx, arrange::duplicateClip(ctx.project, argStr(a, "clipId"), at));
           }});
    r.add({"CopyClip", "Copy Clip To", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::copyClipTo(ctx.project, argStr(a, "clipId"), argStr(a, "trackId"), argNum(a, "startBeat", 0.0)));
           }});
    r.add({"SplitClip", "Split Clip", "Clip", "Ctrl+E", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::splitClip(ctx.project, argStr(a, "clipId"), argNum(a, "atBeat", 0.0)));
           }});
    r.add({"TrimClipStart", "Trim Clip Start", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::trimClipStart(ctx.project, argStr(a, "clipId"), argNum(a, "startBeat", 0.0)));
           }});
    r.add({"TrimClipEnd", "Trim Clip End", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::trimClipEnd(ctx.project, argStr(a, "clipId"), argNum(a, "endBeat", 0.0)));
           }});
    r.add({"SlipClip", "Slip Edit", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::slipClip(ctx.project, argStr(a, "clipId"), argNum(a, "deltaSeconds", 0.0)));
           }});
    r.add({"StretchClip", "Stretch Clip", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::stretchClip(ctx.project, argStr(a, "clipId"), argNum(a, "ratio", 1.0)));
           }});
    r.add({"DeleteClip", "Delete Clip", "Clip", "Delete", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::deleteClip(ctx.project, argStr(a, "clipId")));
           }});
    r.add({"MuteClip", "Mute Clip", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::setClipMuted(ctx.project, argStr(a, "clipId"), argBool(a, "muted", true)));
           }});
    r.add({"LockClip", "Lock Clip", "Clip", "", true, false, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::setClipLocked(ctx.project, argStr(a, "clipId"), argBool(a, "locked", true)));
           }});
    r.add({"ColorClip", "Color Clip", "Clip", "", true, false, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::setClipColor(ctx.project, argStr(a, "clipId"), static_cast<uint32_t>(argNum(a, "color", 0xD4AF37))));
           }});
    r.add({"RenameClip", "Rename Clip", "Clip", "", true, false, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::setClipName(ctx.project, argStr(a, "clipId"), argStr(a, "name")));
           }});
    r.add({"SetClipGain", "Clip Gain", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::setClipGain(ctx.project, argStr(a, "clipId"), static_cast<float>(argNum(a, "gainDb", 0.0))));
           }});
    r.add({"SetFades", "Set Fades", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               std::optional<FadeCurve> ic, oc;
               if (a.contains("inCurve")) ic = static_cast<FadeCurve>(std::clamp(static_cast<int>(argNum(a, "inCurve", 1)), 0, 3));
               if (a.contains("outCurve")) oc = static_cast<FadeCurve>(std::clamp(static_cast<int>(argNum(a, "outCurve", 1)), 0, 3));
               return fromEdit(ctx, arrange::setFades(ctx.project, argStr(a, "clipId"), argNum(a, "fadeInBeats", 0.0),
                                                      argNum(a, "fadeOutBeats", 0.0), ic, oc));
           }});
    r.add({"Crossfade", "Crossfade Clips", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::crossfade(ctx.project, argStr(a, "clipA"), argStr(a, "clipB"), argNum(a, "lengthBeats", 0.25)));
           }});
    r.add({"GroupClips", "Group Clips", "Clip", "Ctrl+G", true, false, [](CommandContext& ctx, const json& a) {
               std::vector<std::string> ids;
               for (auto& x : a.value("clipIds", json::array()))
                   if (x.is_string()) ids.push_back(x.get<std::string>());
               return fromEdit(ctx, arrange::groupClips(ctx.project, ids));
           }});
    r.add({"UngroupClips", "Ungroup", "Clip", "Ctrl+Shift+G", true, false, [](CommandContext& ctx, const json& a) {
               return fromEdit(ctx, arrange::ungroup(ctx.project, argStr(a, "groupId")));
           }});
    r.add({"NormalizeClip", "Normalize Clip", "Clip", "", true, true, [](CommandContext& ctx, const json& a) {
               // Non-destructive: sets clip gain so the clip's peak reaches the target.
               AudioClip* c = ctx.project.findAudioClip(argStr(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               if (c->locked) return fail(ctx, "clip is locked");
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               auto data = ctx.runtime->asset(ctx.project, c->assetId);
               if (!data) return fail(ctx, "audio not loaded");
               const double sr = data->sampleRate;
               const int64_t s0 = static_cast<int64_t>(c->sourceOffsetSec * sr);
               const double durSec = (ctx.project.tempo.beatToSeconds(c->endBeat()) - ctx.project.tempo.beatToSeconds(c->startBeat)) / c->stretch;
               const int64_t s1 = std::min(data->numFrames, s0 + static_cast<int64_t>(durSec * sr));
               float pk = 0.0f;
               for (auto& ch : data->channels)
                   for (int64_t i = std::max<int64_t>(0, s0); i < s1; ++i) pk = std::max(pk, std::fabs(ch[static_cast<size_t>(i)]));
               if (pk <= 1e-9f) return fail(ctx, "clip is silent");
               const double target = argNum(a, "targetDb", -1.0);
               c->gainDb = static_cast<float>(std::clamp(target - gainToDb(static_cast<double>(pk)), -96.0, 48.0));
               ctx.result["gainDb"] = c->gainDb;
               return true;
           }});

    // ---- automation ------------------------------------------------------------
    r.add({"CreateAutomation", "Create Automation Lane", "Automation", "", true, true, [](CommandContext& ctx, const json& a) {
               auto* ch = channelArg(ctx, a);
               if (!ch) return fail(ctx, "channel not found");
               AutomationLane l;
               l.id = files::newId();
               l.channelId = ch->id;
               l.slotId = argStr(a, "slotId");
               l.paramId = argStr(a, "paramId", "gain");
               if (l.slotId.empty()) {
                   if (l.paramId == "tempo") {
                       if (ch->kind != ChannelKind::Master) return fail(ctx, "tempo automation lives on the master channel");
                       if (automation::tempoLane(ctx.project)) return fail(ctx, "the project already has a tempo lane");
                   } else if (l.paramId.rfind("send:", 0) == 0) {
                       const std::string sid = l.paramId.substr(5);
                       if (std::none_of(ch->sends.begin(), ch->sends.end(), [&](const Send& s) { return s.id == sid; }))
                           return fail(ctx, "send not found on this channel");
                   } else if (l.paramId != "gain" && l.paramId != "pan" && l.paramId != "width") {
                       return fail(ctx, "unknown channel parameter " + l.paramId);
                   }
               }
               for (auto& p : a.value("points", json::array()))
                   if (p.is_array() && p.size() >= 2) {
                       AutomationPoint ap{p[0].get<double>(), p[1].get<float>()};
                       if (p.size() >= 3) ap.curve = std::clamp(p[2].get<int>(), 0, 3);
                       if (p.size() >= 4) ap.tension = std::clamp(p[3].get<float>(), -1.0f, 1.0f);
                       l.points.push_back(ap);
                   }
               std::stable_sort(l.points.begin(), l.points.end(), [](auto& x, auto& y) { return x.beat < y.beat; });
               ctx.project.automation.push_back(l);
               automation::applyTempoAutomation(ctx.project);
               ctx.result["id"] = l.id;
               return true;
           }});
    auto curveArg = [](const json& a, int def) {
        if (!a.contains("curve")) return def;
        if (a["curve"].is_number_integer()) return std::clamp(a["curve"].get<int>(), 0, 3);
        const std::string c = a.value("curve", "");
        for (int k = 0; k < 4; ++k)
            if (c == automation::curveName(k)) return k;
        return def;
    };
    r.add({"AddAutomationPoint", "Add Automation Point", "Automation", "", true, true, [curveArg](CommandContext& ctx, const json& a) {
               for (auto& l : ctx.project.automation)
                   if (l.id == argStr(a, "laneId")) {
                       AutomationPoint ap{argNum(a, "beat", 0.0), static_cast<float>(argNum(a, "value", 0.0))};
                       ap.curve = curveArg(a, 0);
                       ap.tension = std::clamp(static_cast<float>(argNum(a, "tension", 0.0)), -1.0f, 1.0f);
                       l.points.push_back(ap);
                       std::stable_sort(l.points.begin(), l.points.end(), [](auto& x, auto& y) { return x.beat < y.beat; });
                       automation::applyTempoAutomation(ctx.project);
                       return true;
                   }
               return fail(ctx, "automation lane not found");
           }});
    r.add({"SetAutomationPoint", "Edit Automation Point", "Automation", "", true, true, [curveArg](CommandContext& ctx, const json& a) {
               for (auto& l : ctx.project.automation)
                   if (l.id == argStr(a, "laneId")) {
                       const int i = static_cast<int>(argNum(a, "index", -1));
                       if (i < 0 || i >= static_cast<int>(l.points.size())) return fail(ctx, "point index out of range");
                       auto& p = l.points[static_cast<size_t>(i)];
                       if (a.contains("beat")) p.beat = std::max(0.0, argNum(a, "beat", p.beat));
                       if (a.contains("value")) p.value = static_cast<float>(argNum(a, "value", p.value));
                       p.curve = curveArg(a, p.curve);
                       if (a.contains("tension")) p.tension = std::clamp(static_cast<float>(argNum(a, "tension", 0.0)), -1.0f, 1.0f);
                       std::stable_sort(l.points.begin(), l.points.end(), [](auto& x, auto& y) { return x.beat < y.beat; });
                       automation::applyTempoAutomation(ctx.project);
                       return true;
                   }
               return fail(ctx, "automation lane not found");
           }});
    r.add({"DeleteAutomationPoint", "Delete Automation Point", "Automation", "", true, true, [](CommandContext& ctx, const json& a) {
               for (auto& l : ctx.project.automation)
                   if (l.id == argStr(a, "laneId")) {
                       const int i = static_cast<int>(argNum(a, "index", -1));
                       if (i < 0 || i >= static_cast<int>(l.points.size())) return fail(ctx, "point index out of range");
                       l.points.erase(l.points.begin() + i);
                       automation::applyTempoAutomation(ctx.project);
                       return true;
                   }
               return fail(ctx, "automation lane not found");
           }});
    r.add({"SetAutomationCurve", "Set Automation Curve", "Automation", "", true, true, [curveArg](CommandContext& ctx, const json& a) {
               for (auto& l : ctx.project.automation)
                   if (l.id == argStr(a, "laneId")) {
                       const int c = curveArg(a, -1);
                       if (c < 0) return fail(ctx, "curve must be Linear, Hold, Smooth or Bezier");
                       const float t = std::clamp(static_cast<float>(argNum(a, "tension", 0.0)), -1.0f, 1.0f);
                       for (auto& p : l.points) {
                           p.curve = c;
                           p.tension = t;
                       }
                       automation::applyTempoAutomation(ctx.project);
                       return true;
                   }
               return fail(ctx, "automation lane not found");
           }});
    r.add({"DeleteAutomation", "Delete Automation Lane", "Automation", "", true, true, [](CommandContext& ctx, const json& a) {
               const AutomationLane* tl = automation::tempoLane(ctx.project);
               const bool hadTempo = tl != nullptr;
               const double startBpm = tl ? automation::laneValueAt(*tl, 0.0).value_or(120.0) : 120.0;
               if (std::erase_if(ctx.project.automation, [&](auto& l) { return l.id == argStr(a, "laneId"); }) == 0)
                   return fail(ctx, "automation lane not found");
               // removing the tempo lane keeps the tempo at the start of the song
               if (hadTempo && !automation::tempoLane(ctx.project)) ctx.project.tempo.setTempo(startBpm);
               return true;
           }});

    // ---- project / timeline ------------------------------------------------------
    r.add({"SetTempo", "Set Tempo", "Project", "", true, true, [](CommandContext& ctx, const json& a) {
               const double bpm = argNum(a, "bpm", 120.0);
               if (bpm < 10 || bpm > 999) return fail(ctx, "tempo must be between 10 and 999 BPM");
               if (a.contains("atBeat")) ctx.project.tempo.addTempoEvent(argNum(a, "atBeat", 0.0), bpm);
               else ctx.project.tempo.setTempo(bpm);
               return true;
           }});
    r.add({"SetTimeSignature", "Set Time Signature", "Project", "", true, true, [](CommandContext& ctx, const json& a) {
               const int num = static_cast<int>(argNum(a, "numerator", 4));
               const int den = static_cast<int>(argNum(a, "denominator", 4));
               if (num < 1 || num > 32 || (den != 1 && den != 2 && den != 4 && den != 8 && den != 16))
                   return fail(ctx, "invalid time signature");
               ctx.project.tempo.setTimeSignature(num, den);
               return true;
           }});
    r.add({"SetKey", "Set Key", "Project", "", true, false, [](CommandContext& ctx, const json& a) {
               auto k = parseKey(argStr(a, "key"));
               if (!k) return fail(ctx, "cannot parse key (e.g. 'F# Minor')");
               ctx.project.key = *k;
               return true;
           }});
    r.add({"SetWrongNoteMode", "Wrong Note Blocker Mode", "MIDI", "", true, false, [](CommandContext& ctx, const json& a) {
               auto m = wrongNoteModeFromId(argStr(a, "mode"));
               if (!m) return fail(ctx, "mode must be off|highlight|snap|block");
               ctx.project.settings.wrongNoteMode = *m;
               return true;
           }});
    r.add({"AddMarker", "Add Marker", "Timeline", "Ctrl+M", true, false, [](CommandContext& ctx, const json& a) {
               ctx.result["id"] = arrange::addMarker(ctx.project, argNum(a, "beat", 0.0), argStr(a, "name", "Marker"));
               return true;
           }});
    r.add({"RemoveMarker", "Remove Marker", "Timeline", "", true, false, [](CommandContext& ctx, const json& a) {
               return arrange::removeMarker(ctx.project, argStr(a, "markerId")) || fail(ctx, "marker not found");
           }});
    r.add({"AddSection", "Add Section", "Timeline", "", true, false, [](CommandContext& ctx, const json& a) {
               ctx.result["id"] = arrange::addSection(ctx.project, argStr(a, "name", "Section"), argStr(a, "type", "verse"),
                                                      argNum(a, "startBeat", 0.0), argNum(a, "endBeat", 16.0));
               return true;
           }});
    r.add({"SetLoop", "Set Loop Region", "Timeline", "", true, false, [](CommandContext& ctx, const json& a) {
               arrange::setLoop(ctx.project, argBool(a, "enabled", true), argNum(a, "startBeat", 0.0), argNum(a, "endBeat", 16.0));
               return true;
           }});
    r.add({"SetMetronome", "Metronome", "Transport", "", true, false, [](CommandContext& ctx, const json& a) {
               ctx.project.settings.metronome = argBool(a, "enabled", !ctx.project.settings.metronome);
               ctx.project.settings.countInBars = static_cast<int>(argNum(a, "countInBars", ctx.project.settings.countInBars));
               return true;
           }});
    r.add({"RenameProject", "Rename Project", "Project", "", true, false, [](CommandContext& ctx, const json& a) {
               ctx.project.name = argStr(a, "name", ctx.project.name);
               return true;
           }});
    // ---- MIDI / piano roll ------------------------------------------------------
    auto noteSel = [](const json& a) {
        midi::Selection s;
        for (auto& x : a.value("notes", json::array()))
            if (x.is_number_integer() && x.get<long long>() >= 0) s.push_back(static_cast<size_t>(x.get<long long>()));
        return s;
    };
    auto midiClip = [](CommandContext& ctx, const json& a) -> MidiClip* {
        MidiClip* c = ctx.project.findMidiClip(argStr(a, "clipId"));
        if (!c) ctx.error = "MIDI clip not found";
        else if (c->locked) {
            ctx.error = "clip is locked";
            return nullptr;
        }
        return c;
    };
    r.add({"AddMidiClip", "Add MIDI Clip", "MIDI", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t || t->type != TrackType::Midi) return fail(ctx, "MIDI track not found");
               MidiClip c;
               c.id = files::newId();
               c.name = argStr(a, "name", "Pattern");
               c.startBeat = std::max(0.0, argNum(a, "startBeat", 0.0));
               c.lengthBeats = std::max(0.25, argNum(a, "lengthBeats", 4.0));
               c.loopLengthBeats = std::max(0.0, argNum(a, "loopLengthBeats", 0.0));
               t->midiClips.push_back(c);
               ctx.result["id"] = c.id;
               return true;
           }});
    // A live MIDI take -> one new clip (one undo step). events: [[timelineSample, status, data1, data2], ...]
    r.add({"AddMidiRecording", "Record MIDI", "MIDI", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(argStr(a, "trackId"));
               if (!t || t->type != TrackType::Midi) return fail(ctx, "MIDI track not found");
               std::vector<midi::TimedMidi> ev;
               for (auto& e : a.value("events", json::array()))
                   if (e.is_array() && e.size() == 4 && e[0].is_number_integer())
                       ev.push_back({e[0].get<int64_t>(), static_cast<uint8_t>(e[1].get<int>()), static_cast<uint8_t>(e[2].get<int>()),
                                     static_cast<uint8_t>(e[3].get<int>())});
               const double sr = argNum(a, "sampleRate", 48000.0);
               auto clip = midi::clipFromLiveRecording(ev, ctx.project.tempo, sr, static_cast<int64_t>(argNum(a, "endTimeline", 0.0)),
                                                       argNum(a, "quantizeBeats", 0.0));
               if (!clip) return fail(ctx, "no notes were played");
               clip->id = files::newId();
               t->midiClips.push_back(*clip);
               ctx.result["id"] = clip->id;
               ctx.result["notes"] = clip->notes.size();
               ctx.result["startBeat"] = clip->startBeat;
               return true;
           }});
    r.add({"AddNote", "Add Note", "MIDI", "", true, true, [midiClip](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               MidiNote n;
               n.pitch = static_cast<int>(argNum(a, "pitch", 60));
               n.velocity = static_cast<int>(argNum(a, "velocity", 100));
               n.startBeat = argNum(a, "startBeat", 0.0);
               n.lengthBeats = argNum(a, "lengthBeats", 1.0);
               n.slide = argBool(a, "slide", false);
               const auto mode = a.contains("wrongNoteMode") ? wrongNoteModeFromId(argStr(a, "wrongNoteMode")).value_or(WrongNoteMode::Off)
                                                             : ctx.project.settings.wrongNoteMode;
               auto idx = midi::addNote(*c, n, ctx.project.key, mode);
               if (!idx) return fail(ctx, "note blocked: " + noteName(n.pitch) + " is not in " + ctx.project.key.name());
               ctx.result["index"] = *idx;
               ctx.result["pitch"] = c->notes[*idx].pitch;
               ctx.result["outOfKey"] = !ctx.project.key.contains(c->notes[*idx].pitch);
               return true;
           }});
    r.add({"DeleteNotes", "Delete Notes", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::removeNotes(*c, noteSel(a));
               return true;
           }});
    r.add({"QuantizeNotes", "Quantize", "MIDI", "Q", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::quantize(*c, noteSel(a), argNum(a, "grid", 0.25), argNum(a, "strength", 1.0), argBool(a, "ends", false), argNum(a, "swing", 0.0));
               return true;
           }});
    r.add({"HumanizeNotes", "Humanize", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::humanize(*c, noteSel(a), argNum(a, "timing", 0.02), static_cast<int>(argNum(a, "velocity", 8)), static_cast<uint64_t>(argNum(a, "seed", 1)));
               return true;
           }});
    r.add({"TransposeNotes", "Transpose", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               const bool diatonic = argBool(a, "diatonic", false);
               midi::transpose(*c, noteSel(a), static_cast<int>(argNum(a, "amount", 0)), diatonic ? &ctx.project.key : nullptr);
               return true;
           }});
    r.add({"DuplicateNotes", "Duplicate Notes", "MIDI", "Ctrl+B", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               auto created = midi::duplicate(*c, noteSel(a), static_cast<int>(argNum(a, "times", 1)));
               ctx.result["created"] = created;
               return true;
           }});
    r.add({"LegatoNotes", "Legato", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::legato(*c, noteSel(a));
               return true;
           }});
    r.add({"StrumNotes", "Strum", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::strum(*c, noteSel(a), argNum(a, "step", 0.03), argBool(a, "up", true));
               return true;
           }});
    r.add({"ArpeggiateNotes", "Arpeggiate", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               const std::string m = argStr(a, "mode", "up");
               const midi::ArpMode mode = m == "down" ? midi::ArpMode::Down : m == "updown" ? midi::ArpMode::UpDown
                                        : m == "random" ? midi::ArpMode::Random : m == "played" ? midi::ArpMode::AsPlayed : midi::ArpMode::Up;
               midi::arpeggiate(*c, noteSel(a), argNum(a, "rate", 0.25), mode, argNum(a, "gate", 0.9), static_cast<uint64_t>(argNum(a, "seed", 1)));
               return true;
           }});
    r.add({"SnapNotesToKey", "Snap Notes To Key", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               ctx.result["changed"] = midi::snapToKey(*c, noteSel(a), ctx.project.key);
               return true;
           }});
    r.add({"SetNoteVelocity", "Set Velocity", "MIDI", "", true, true, [midiClip, noteSel](CommandContext& ctx, const json& a) {
               MidiClip* c = midiClip(ctx, a);
               if (!c) return false;
               midi::setVelocity(*c, noteSel(a), static_cast<int>(argNum(a, "velocity", 100)));
               return true;
           }});
    r.add({"ImportMidi", "Import MIDI File", "MIDI", "", true, true, [](CommandContext& ctx, const json& a) {
               midi::SmfData d;
               std::string err;
               if (!midi::readMidiFile(argStr(a, "path"), d, &err)) return fail(ctx, err);
               ctx.result["tracks"] = midi::importSmfIntoProject(ctx.project, d, argNum(a, "atBeat", 0.0), argBool(a, "applyTempo", false));
               return true;
           }});
    r.add({"ExportMidi", "Export MIDI File", "MIDI", "", false, false, [](CommandContext& ctx, const json& a) {
               std::string err;
               midi::SmfData d;
               if (a.contains("clipId")) {
                   const MidiClip* c = ctx.project.findMidiClip(argStr(a, "clipId"));
                   if (!c) return fail(ctx, "MIDI clip not found");
                   d = midi::clipToSmf(ctx.project, *c, c->name);
               } else {
                   d = midi::projectToSmf(ctx.project);
               }
               if (!midi::writeMidiFile(argStr(a, "path"), d, false, &err)) return fail(ctx, err);
               return true;
           }});
    registerVocalCommands(r);
    registerProductionCommands(r);
    registerMasterCommands(r);
    registerIntelligenceCommands(r);
    registerPluginCommands(r);
    registerBeatCommands(r);
}

} // namespace roy
