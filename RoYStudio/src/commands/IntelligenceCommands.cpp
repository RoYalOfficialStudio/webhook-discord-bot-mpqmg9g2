// RoY Intelligence commands: mix analysis, energy map, sections, project check, Vocal DNA.
// Analyses never change the project; ApplySections/LearnVocalDna are explicit, undoable writes.
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "intelligence/Arrangement.h"
#include "intelligence/Assistant.h"
#include "intelligence/MixIntelligence.h"

namespace roy {

namespace {
bool fail(CommandContext& ctx, const std::string& e) {
    ctx.error = e;
    return false;
}
double num(const json& a, const char* k, double d) {
    auto it = a.find(k);
    return it != a.end() && it->is_number() ? it->get<double>() : d;
}
bool renderAll(CommandContext& ctx, const json& a, std::vector<mixi::TrackAudio>& tracks, mixi::Channels& master) {
    if (!ctx.runtime) return fail(ctx, "audio runtime not available");
    const double s = num(a, "startBeat", 0.0), e = num(a, "endBeat", ctx.project.endBeat());
    if (e <= s) return fail(ctx, "empty range");
    std::string err;
    if (!mixi::renderForAnalysis(ctx.runtime->engine(), *ctx.runtime, ctx.project, s, e, tracks, master, &err)) return fail(ctx, err);
    return true;
}
} // namespace

void registerIntelligenceCommands(CommandRegistry& r) {
    r.add({"AnalyzeMix", "Mix Intelligence", "Intelligence", "", false, false, [](CommandContext& ctx, const json& a) {
               std::vector<mixi::TrackAudio> tracks;
               mixi::Channels master;
               if (!renderAll(ctx, a, tracks, master)) return false;
               mixi::MixSettings ms;
               ms.sampleRate = ctx.runtime->engine().sampleRate();
               ctx.result = mixi::analyzeMix(tracks, master, ms).toJson();
               return true;
           }});
    r.add({"EnergyMap", "Energy Map", "Intelligence", "", false, false, [](CommandContext& ctx, const json& a) {
               std::vector<mixi::TrackAudio> tracks;
               mixi::Channels master;
               if (!renderAll(ctx, a, tracks, master)) return false;
               auto map = arrangei::buildEnergyMap(master, tracks, ctx.runtime->engine().sampleRate(), ctx.project.tempo, num(a, "startBeat", 0.0));
               json bars = json::array();
               for (auto& b : map.bars)
                   bars.push_back({{"startBeat", b.startBeat}, {"endBeat", b.endBeat}, {"lufs", b.loudnessLufs}, {"density", b.density},
                                   {"centroidHz", b.spectralCentroid}, {"drums", b.drumActivity}, {"bass", b.bassActivity},
                                   {"vocal", b.vocalActivity}, {"energy", b.energy}});
               json sections = json::array();
               for (auto& s : arrangei::suggestSections(map, static_cast<int>(num(a, "minBars", 4))))
                   sections.push_back({{"name", s.name}, {"type", s.type}, {"startBeat", s.startBeat}, {"endBeat", s.endBeat},
                                       {"cluster", s.cluster}, {"energy", s.meanEnergy}, {"confidence", s.confidence}});
               ctx.result = {{"bars", bars}, {"sections", sections}, {"note", map.note}};
               return true;
           }});
    r.add({"ApplySections", "Apply Section Suggestions", "Timeline", "", true, false, [](CommandContext& ctx, const json& a) {
               const auto list = a.value("sections", json::array());
               if (list.empty()) return fail(ctx, "no sections given");
               if (a.value("replace", false)) ctx.project.sections.clear();
               for (auto& s : list) {
                   Section x;
                   x.id = files::newId();
                   x.name = s.value("name", "Section");
                   x.type = s.value("type", "other");
                   x.startBeat = s.value("startBeat", 0.0);
                   x.endBeat = s.value("endBeat", x.startBeat + 16.0);
                   ctx.project.sections.push_back(x);
               }
               std::sort(ctx.project.sections.begin(), ctx.project.sections.end(), [](auto& x, auto& y) { return x.startBeat < y.startBeat; });
               return true;
           }});
    r.add({"ProjectCheck", "Project Assistant", "Intelligence", "", false, false, [](CommandContext& ctx, const json& a) {
               auto f = assist::checkProject(ctx.project, ctx.projectFolder, a.value("dirty", false), static_cast<size_t>(num(a, "backups", 0)));
               json out = json::array();
               for (auto& x : f) out.push_back({{"id", x.id}, {"severity", x.severity}, {"text", x.text}, {"command", x.command}, {"args", x.args}});
               ctx.result = {{"findings", out}};
               return true;
           }});
    r.add({"LearnVocalDna", "Learn Vocal DNA", "Vocal", "", true, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               AudioClip* c = ctx.project.findAudioClip(a.value("clipId", ""));
               if (!c) return fail(ctx, "audio clip not found");
               auto d = ctx.runtime->asset(ctx.project, c->assetId);
               if (!d) return fail(ctx, "audio not available");
               auto dna = assist::VocalDna::fromJson(ctx.project.vocalSettings.value("dna", json::object()));
               assist::learnVocal(dna, d->channels, d->sampleRate);
               ctx.project.vocalSettings["dna"] = dna.toJson();
               ctx.result = dna.toJson();
               return true;
           }});
    r.add({"CompareVocalDna", "Compare With Vocal DNA", "Vocal", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               AudioClip* c = ctx.project.findAudioClip(a.value("clipId", ""));
               if (!c) return fail(ctx, "audio clip not found");
               auto d = ctx.runtime->asset(ctx.project, c->assetId);
               if (!d) return fail(ctx, "audio not available");
               auto dna = assist::VocalDna::fromJson(ctx.project.vocalSettings.value("dna", json::object()));
               json out = json::array();
               for (auto& dev : assist::compareToDna(dna, d->channels, d->sampleRate))
                   out.push_back({{"key", dev.key}, {"text", dev.text}, {"value", dev.value}, {"usual", dev.usual}});
               ctx.result = {{"deviations", out}};
               return true;
           }});
}

} // namespace roy
