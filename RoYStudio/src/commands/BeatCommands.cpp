// Beat Lab commands: patterns, steps, swing, pattern clips (drum programming).
#include "commands/Commands.h"
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
} // namespace

void registerBeatCommands(CommandRegistry& r) {
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
               for (size_t i = 0; i < row->steps.size(); ++i) {
                   const char c = t[i % t.size()];
                   row->steps[i].on = c == 'x' || c == 'X' || c == 'o';
                   row->steps[i].velocity = c == 'o' ? 0.45f : (c == 'X' ? 1.0f : 0.8f);
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
               t->instrument = slot;
               ctx.result["id"] = slot.id;
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
