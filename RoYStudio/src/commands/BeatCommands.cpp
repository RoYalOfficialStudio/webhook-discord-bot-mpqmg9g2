// Beat Lab commands: patterns, steps, swing, pattern clips (drum programming).
#include "commands/Commands.h"
#include "beat/Collision.h"
#include "beat/StepSequencer.h"
#include "core/AudioBuffer.h"
#include "instruments/Drums.h"
#include "core/Files.h"

#include <algorithm>

namespace roy {

namespace {
bool fail(CommandContext& ctx, const std::string& e) {
    ctx.error = e;
    return false;
}
std::string str(const json& a, const char* k, const std::string& d = {}) {
    auto it = a.find(k);
    return it != a.end() && it->is_string() ? it->get<std::string>() : d;
}
double num(const json& a, const char* k, double d) {
    auto it = a.find(k);
    return it != a.end() && it->is_number() ? it->get<double>() : d;
}
Pattern* patternArg(CommandContext& ctx, const json& a) {
    for (auto& p : ctx.project.patterns)
        if (p.id == str(a, "patternId")) return &p;
    ctx.error = "pattern not found";
    return nullptr;
}
// Row by id ("rowId"), index ("row") or voice name ("voice": "kick").
PatternRow* rowArg(CommandContext& ctx, Pattern& p, const json& a) {
    if (a.contains("rowId"))
        for (auto& r : p.rows)
            if (r.id == str(a, "rowId")) return &r;
    if (a.contains("voice"))
        for (auto& r : p.rows)
            if (r.voice == str(a, "voice")) return &r;
    if (a.contains("row")) {
        const int i = static_cast<int>(num(a, "row", -1));
        if (i >= 0 && i < static_cast<int>(p.rows.size())) return &p.rows[static_cast<size_t>(i)];
    }
    ctx.error = "pattern row not found";
    return nullptr;
}
// Renders a one-shot (note on at 0, off after holdSec) of an instrument, mono (L+R)/2.
std::vector<float> renderOneShot(Processor& inst, int note, float velocity, double sr, double holdSec, double lengthSec) {
    const int block = 256;
    inst.prepare(sr, block);
    AudioBuffer buf(2, block);
    const int64_t total = static_cast<int64_t>(lengthSec * sr), offAt = static_cast<int64_t>(holdSec * sr);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(total));
    for (int64_t pos = 0; pos < total; pos += block) {
        buf.clear();
        std::vector<NoteEvent> ev;
        if (pos == 0) {
            NoteEvent e;
            e.note = static_cast<int16_t>(note);
            e.velocity = velocity;
            ev.push_back(e);
        }
        if (offAt >= pos && offAt < pos + block) {
            NoteEvent e;
            e.type = NoteEvent::NoteOff;
            e.offset = static_cast<int>(offAt - pos);
            e.note = static_cast<int16_t>(note);
            ev.push_back(e);
        }
        auto blk = buf.block();
        inst.process(blk, nullptr, ev.data(), static_cast<int>(ev.size()));
        const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        for (int i = 0; i < n; ++i) out.push_back(0.5f * (buf.channel(0)[i] + buf.channel(1)[i]));
    }
    return out;
}
} // namespace

void registerBeatCommands(CommandRegistry& r) {
    // KICK <-> 808 analyzer on the project's own sounds: the kick row of the beat track
    // (its sample or drum voice) and the 808 track's instrument with its current settings,
    // offset as arranged (first 808 note vs. the kick hit before it). Measures only.
    r.add({"AnalyzeKick808", "Analyze Kick vs 808", "Beat", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "no audio runtime");
               const double sr = ctx.runtime->engine().sampleRate() > 0 ? ctx.runtime->engine().sampleRate() : 48000.0;
               Project& p = ctx.project;
               const Track* beatTrack = nullptr;
               const Track* bassTrack = nullptr;
               for (auto& t : p.tracks) {
                   if (t.type == TrackType::Beat && (!beatTrack || t.id == str(a, "beatTrackId"))) beatTrack = &t;
                   if (t.instrument && t.instrument->typeId == "roy.808" && (!bassTrack || t.id == str(a, "bassTrackId"))) bassTrack = &t;
               }
               if (!beatTrack || !bassTrack) return fail(ctx, "needs a beat track with a kick and an 808 track");
               // kick sound
               const PatternRow* kickRow = nullptr;
               const Pattern* kickPat = nullptr;
               for (auto& pc : beatTrack->patternClips)
                   if (const Pattern* pat = p.findPattern(pc.patternId))
                       for (auto& row : pat->rows)
                           if (row.voice == "kick" && !kickRow) {
                               kickRow = &row;
                               kickPat = pat;
                           }
               if (!kickRow)
                   for (auto& pat : p.patterns)
                       for (auto& row : pat.rows)
                           if (row.voice == "kick" && !kickRow) {
                               kickRow = &row;
                               kickPat = &pat;
                           }
               if (!kickRow) return fail(ctx, "no kick row found");
               auto drums = std::unique_ptr<Processor>(ProcessorFactory::instance().create("roy.drums"));
               if (!drums) return fail(ctx, "drum instrument unavailable");
               if (beatTrack->instrument && beatTrack->instrument->typeId == "roy.drums") drums->loadState(beatTrack->instrument->state);
               if (!kickRow->sampleAssetId.empty())
                   if (auto* d = dynamic_cast<RoyDrums*>(drums.get())) d->setSample(kickRow->note, ctx.runtime->asset(p, kickRow->sampleAssetId));
               const auto kick = renderOneShot(*drums, kickRow->note, std::clamp(kickRow->volume, 0.1f, 1.0f), sr, 0.05, 0.8);
               // 808 sound: first note of the 808 track (or C1)
               int bassNote = 36;
               double bassBeat = -1, bassLen = 1.0;
               for (auto& c : bassTrack->midiClips)
                   for (auto& n : c.notes)
                       if (!n.muted && (bassBeat < 0 || c.startBeat + n.startBeat < bassBeat)) {
                           bassBeat = c.startBeat + n.startBeat;
                           bassNote = n.pitch;
                           bassLen = n.lengthBeats;
                       }
               auto bass808 = std::unique_ptr<Processor>(ProcessorFactory::instance().create("roy.808"));
               if (!bass808) return fail(ctx, "808 instrument unavailable");
               bass808->loadState(bassTrack->instrument->state);
               const double holdSec = std::clamp(p.tempo.beatToSeconds(bassBeat < 0 ? 0 : bassBeat + bassLen) -
                                                     p.tempo.beatToSeconds(bassBeat < 0 ? 0 : bassBeat), 0.05, 2.0);
               const auto bass = renderOneShot(*bass808, bassNote, 1.0f, sr, holdSec, 1.2);
               // arranged offset: 808 start relative to the latest kick hit at or before it (within one beat)
               double offset = num(a, "offsetMs", -1.0) >= 0 ? num(a, "offsetMs", 0.0) / 1000.0 : 0.0;
               if (num(a, "offsetMs", -1.0) < 0 && bassBeat >= 0) {
                   double best = -1;
                   for (auto& pc : beatTrack->patternClips)
                       if (const Pattern* pat = p.findPattern(pc.patternId))
                           for (auto& n : expandPatternClip(*pat, pc, 1))
                               if (n.note == kickRow->note && n.beat <= bassBeat + 1e-9 && bassBeat - n.beat <= 1.0) best = std::max(best, n.beat);
                   if (best >= 0) offset = p.tempo.beatToSeconds(bassBeat) - p.tempo.beatToSeconds(best);
               }
               const auto report = beat::analyzeKick808(kick, bass, sr, offset);
               const auto visual = beat::analyzeKick808Visual(kick, bass, sr, offset);
               ctx.result = report.toJson();
               ctx.result["visual"] = visual.toJson();
               ctx.result["offsetMs"] = offset * 1000.0;
               ctx.result["kickNote"] = kickRow->note;
               ctx.result["bassNote"] = bassNote;
               ctx.result["patternName"] = kickPat ? kickPat->name : "";
               return true;
           }});
    r.add({"DetectRoot", "Detect Root Note", "Beat", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "no audio runtime");
               auto data = ctx.runtime->asset(ctx.project, str(a, "assetId"));
               if (!data || data->channels.empty() || data->channels[0].empty()) return fail(ctx, "asset not found or empty");
               const auto rd = beat::detectRoot(data->channels[0].data(), static_cast<int64_t>(data->channels[0].size()), data->sampleRate);
               if (rd.midiNote < 0) return fail(ctx, "no clear pitch found");
               ctx.result = rd.toJson();
               return true;
           }});
    r.add({"AddPattern", "New Pattern", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               const int steps = static_cast<int>(num(a, "steps", 16));
               if (steps != 16 && steps != 32 && steps != 64) return fail(ctx, "steps must be 16, 32 or 64");
               Pattern p = makeDefaultPattern(str(a, "name", "Pattern " + std::to_string(ctx.project.patterns.size() + 1)), steps);
               ctx.project.patterns.push_back(p);
               ctx.result["id"] = p.id;
               json rows = json::array();
               for (auto& row : p.rows) rows.push_back({{"id", row.id}, {"voice", row.voice}, {"note", row.note}});
               ctx.result["rows"] = rows;
               return true;
           }});
    r.add({"SetStep", "Set Step", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               const int i = static_cast<int>(num(a, "step", -1));
               if (i < 0 || i >= static_cast<int>(row->steps.size())) return fail(ctx, "step out of range");
               Step& s = row->steps[static_cast<size_t>(i)];
               s.on = a.value("on", !s.on);
               s.velocity = static_cast<float>(std::clamp(num(a, "velocity", s.velocity), 0.0, 1.0));
               s.pan = static_cast<float>(std::clamp(num(a, "pan", s.pan), -1.0, 1.0));
               s.pitch = static_cast<float>(std::clamp(num(a, "pitch", s.pitch), -24.0, 24.0));
               s.probability = static_cast<float>(std::clamp(num(a, "probability", s.probability), 0.0, 1.0));
               s.microTiming = static_cast<float>(std::clamp(num(a, "microTiming", s.microTiming), -0.5, 0.5));
               s.flam = a.value("flam", s.flam);
               s.roll = std::clamp(static_cast<int>(num(a, "roll", s.roll)), 0, 8);
               s.rollLength = std::clamp(static_cast<int>(num(a, "rollLength", s.rollLength)), 1, 16);
               return true;
           }});
    // Fills a row from a pattern string like "x...x...x...x..." (x = on, o = soft, . = off).
    r.add({"SetRowPattern", "Set Row From Text", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               const std::string t = str(a, "text");
               if (t.empty()) return fail(ctx, "text required");
               // The text defines the whole row: plain hits (no leftover ratchets, flams, odds).
               for (size_t i = 0; i < row->steps.size(); ++i) {
                   const char c = t[i % t.size()];
                   Step s;
                   s.on = c == 'x' || c == 'X' || c == 'o';
                   s.velocity = c == 'o' ? 0.45f : (c == 'X' ? 1.0f : 0.8f);
                   row->steps[i] = s;
               }
               return true;
           }});
    r.add({"SetPatternSwing", "Set Swing", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               p->swing = static_cast<float>(std::clamp(num(a, "swing", 0.0), 0.0, 1.0));
               return true;
           }});
    r.add({"SetRowMix", "Set Drum Row Mix", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               row->volume = static_cast<float>(std::clamp(num(a, "volume", row->volume), 0.0, 1.0));
               row->pan = static_cast<float>(std::clamp(num(a, "pan", row->pan), -1.0, 1.0));
               row->pitch = static_cast<float>(std::clamp(num(a, "pitch", row->pitch), -24.0, 24.0));
               row->muted = a.value("muted", row->muted);
               row->solo = a.value("solo", row->solo);
               return true;
           }});
    // Drum pad sample: the row plays this asset instead of the synthesized drum voice.
    r.add({"SetRowSample", "Set Drum Pad Sample", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               const std::string asset = str(a, "assetId");
               if (!asset.empty() && !ctx.project.findAsset(asset)) return fail(ctx, "asset not found");
               row->sampleAssetId = asset;
               if (a.contains("name") && !str(a, "name").empty()) row->name = str(a, "name");
               return true;
           }});
    // Sampler: map one sample chromatically (root note) on a MIDI track (switches it to RoY Sampler).
    r.add({"LoadSampleIntoSampler", "Load Sample Into Sampler", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(str(a, "trackId"));
               if (!t || t->type == TrackType::Audio) return fail(ctx, "MIDI/beat track required");
               const std::string asset = str(a, "assetId");
               if (!ctx.project.findAsset(asset)) return fail(ctx, "asset not found");
               PluginSlot slot;
               slot.id = t->instrument && t->instrument->typeId == "roy.sampler" ? t->instrument->id : files::newId();
               slot.typeId = "roy.sampler";
               slot.name = "RoY Sampler";
               slot.state = t->instrument && t->instrument->typeId == "roy.sampler" ? t->instrument->state : json::object();
               if (!a.value("append", false)) slot.state["zones"] = json::array();
               slot.state["zones"].push_back({{"assetId", asset}, {"root", static_cast<int>(num(a, "rootNote", 60))}, {"lo", 0}, {"hi", 127},
                                             {"loVel", 1}, {"hiVel", 127}, {"start", 0}, {"end", -1}, {"loop", 0}, {"loopStart", 0},
                                             {"loopEnd", -1}, {"oneShot", a.value("oneShot", false)}, {"reverse", false}, {"gainDb", 0.0},
                                             {"tune", 0.0}, {"pan", 0.0}, {"choke", 0}});
               if (a.value("autoRoot", false) && ctx.runtime) { // tune the zone from the detected root note
                   if (auto data = ctx.runtime->asset(ctx.project, asset); data && !data->channels.empty()) {
                       const auto rd = beat::detectRoot(data->channels[0].data(), static_cast<int64_t>(data->channels[0].size()), data->sampleRate);
                       if (rd.midiNote >= 0 && rd.confidence > 0.2) {
                           auto& z = slot.state["zones"].back();
                           z["root"] = rd.midiNote;
                           z["tune"] = -rd.cents / 100.0; // play in tune, not just on the nearest note
                           ctx.result["root"] = rd.toJson();
                       }
                   }
               }
               t->instrument = slot;
               ctx.result["id"] = slot.id;
               return true;
           }});
    r.add({"SetPatternGroove", "Set Groove", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               const std::string g = str(a, "groove", p->groove);
               if (!beat::findGroove(g)) return fail(ctx, "unknown groove template " + g);
               p->groove = g;
               p->grooveAmount = static_cast<float>(std::clamp(num(a, "amount", p->grooveAmount), 0.0, 1.0));
               return true;
           }});
    r.add({"SetVelocityCurve", "Velocity Curve", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               if (!beat::applyVelocityCurve(*row, str(a, "curve", "Flat"), static_cast<float>(num(a, "lo", 0.4)),
                                             static_cast<float>(num(a, "hi", 1.0)), static_cast<int>(num(a, "from", 0)),
                                             static_cast<int>(num(a, "to", -1)), static_cast<uint64_t>(num(a, "seed", 1))))
                   return fail(ctx, "unknown velocity curve " + str(a, "curve"));
               return true;
           }});
    r.add({"NoteRepeat", "Note Repeat", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternRow* row = rowArg(ctx, *p, a);
               if (!row) return false;
               std::string err;
               if (!beat::noteRepeat(*p, *row, static_cast<int>(num(a, "from", 0)), static_cast<int>(num(a, "to", -1)),
                                     str(a, "rate", "1/16"), static_cast<float>(num(a, "velocity", 0.8)), &err))
                   return fail(ctx, err);
               return true;
           }});
    // One command = one undo step, however many steps it writes.
    r.add({"GeneratePattern", "Generate Pattern", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               const int steps = static_cast<int>(num(a, "steps", 16));
               if (steps != 16 && steps != 32 && steps != 64) return fail(ctx, "steps must be 16, 32 or 64");
               const std::string style = str(a, "style", "Trap");
               Pattern p = makeDefaultPattern(str(a, "name", style + " " + std::to_string(ctx.project.patterns.size() + 1)), steps);
               std::string err;
               if (!beat::generatePattern(p, style, static_cast<uint64_t>(num(a, "seed", 1)), &err)) return fail(ctx, err);
               ctx.project.patterns.push_back(p);
               ctx.result["id"] = p.id;
               return true;
           }});
    r.add({"MakeVariation", "Make Variation", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               Pattern v = beat::makeVariation(*p, static_cast<uint64_t>(num(a, "seed", 1)), static_cast<float>(num(a, "amount", 0.3)));
               if (a.contains("name")) v.name = str(a, "name", v.name);
               ctx.project.patterns.push_back(v);
               ctx.result["id"] = v.id;
               return true;
           }});
    // Pattern chaining: A B A C ... placed back to back on a beat track (one undo step).
    r.add({"PlacePatternChain", "Place Pattern Chain", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(str(a, "trackId"));
               if (!t || t->type != TrackType::Beat) return fail(ctx, "beat track not found");
               const auto ids = a.value("patternIds", json::array());
               if (!ids.is_array() || ids.empty()) return fail(ctx, "patternIds required");
               const int repeats = std::clamp(static_cast<int>(num(a, "repeats", 1)), 1, 256);
               double at = std::max(0.0, num(a, "startBeat", 0.0));
               json placed = json::array();
               for (int rep = 0; rep < repeats; ++rep)
                   for (auto& id : ids) {
                       const Pattern* pat = nullptr;
                       for (auto& p : ctx.project.patterns)
                           if (id.is_string() && p.id == id.get<std::string>()) pat = &p;
                       if (!pat) return fail(ctx, "pattern in chain not found");
                       PatternClip c;
                       c.id = files::newId();
                       c.patternId = pat->id;
                       c.startBeat = at;
                       c.lengthBeats = pat->lengthBeats();
                       at += c.lengthBeats;
                       t->patternClips.push_back(c);
                       placed.push_back(c.id);
                   }
               ctx.result["ids"] = placed;
               ctx.result["endBeat"] = at;
               return true;
           }});
    r.add({"AddPatternClip", "Place Pattern", "Beat", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* t = ctx.project.findTrack(str(a, "trackId"));
               if (!t || t->type != TrackType::Beat) return fail(ctx, "beat track not found");
               Pattern* p = patternArg(ctx, a);
               if (!p) return false;
               PatternClip c;
               c.id = files::newId();
               c.patternId = p->id;
               c.startBeat = std::max(0.0, num(a, "startBeat", 0.0));
               c.lengthBeats = std::max(0.25, num(a, "lengthBeats", p->lengthBeats()));
               t->patternClips.push_back(c);
               ctx.result["id"] = c.id;
               return true;
           }});
}

} // namespace roy
