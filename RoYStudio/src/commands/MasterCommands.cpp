// Mastering and export commands.
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "export/Exporter.h"
#include "master/Mastering.h"

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
} // namespace

void registerMasterCommands(CommandRegistry& r) {
    r.add({"CreateMasterChain", "Create Master Chain", "Master", "", true, true, [](CommandContext& ctx, const json& a) {
               const master::MasterPreset* p = master::findPreset(str(a, "preset", "streaming"));
               if (!p) return fail(ctx, "unknown master preset");
               MixerChannel* m = ctx.project.master();
               if (!m) return fail(ctx, "no master channel");
               json ids = json::array();
               for (auto& mod : p->chain) {
                   PluginSlot s;
                   s.id = files::newId();
                   s.typeId = mod.typeId;
                   s.name = mod.name;
                   s.state = json::object();
                   s.state["params"] = mod.params.is_object() ? mod.params : json::object();
                   m->inserts.push_back(s); // appended: existing master inserts are kept
                   ids.push_back(s.id);
               }
               ctx.project.presets["masterPreset"] = p->id;
               ctx.result["slots"] = ids;
               return true;
           }});
    r.add({"Export", "Export", "Export", "Ctrl+Shift+E", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               exporting::ExportOptions o;
               const std::string fmt = str(a, "format", "wav");
               o.format = fmt == "flac" ? exporting::Format::Flac : fmt == "mp3" ? exporting::Format::Mp3 : exporting::Format::Wav;
               o.sampleRate = num(a, "sampleRate", 0.0);
               o.bitDepth = static_cast<int>(num(a, "bitDepth", 24));
               const std::string d = str(a, "dither", "tpdf");
               o.dither = d == "none" ? exporting::Dither::None : d == "shaped" ? exporting::Dither::TpdfShaped : exporting::Dither::Tpdf;
               const std::string nm = str(a, "normalize", "none");
               o.normalize = nm == "peak" ? exporting::Normalize::Peak : nm == "loudness" ? exporting::Normalize::Loudness : exporting::Normalize::None;
               o.normalizePeakDb = num(a, "peakDb", -1.0);
               o.normalizeLufs = num(a, "lufs", -14.0);
               o.truePeakCeilingDb = num(a, "ceilingDb", -1.0);
               o.tailSeconds = num(a, "tailSeconds", -1.0);
               const std::string range = str(a, "range", "song");
               o.range = range == "selection" ? exporting::Range::Selection : range == "loop" ? exporting::Range::Loop : exporting::Range::FullSong;
               o.startBeat = num(a, "startBeat", 0.0);
               o.endBeat = num(a, "endBeat", 0.0);
               const std::string st = str(a, "stems", "none");
               o.stems = st == "tracks" ? exporting::Stems::AllTracks : st == "selected" ? exporting::Stems::SelectedTracks
                       : st == "busses" ? exporting::Stems::MixerBusses : st == "vocals" ? exporting::Stems::VocalStems
                       : st == "instrumental" ? exporting::Stems::Instrumental : exporting::Stems::None;
               o.mixdown = a.value("mixdown", true);
               for (auto& id : a.value("tracks", json::array()))
                   if (id.is_string()) o.selectedTrackIds.push_back(id.get<std::string>());
               o.folder = str(a, "folder", ctx.projectFolder.empty() ? std::string() : (ctx.projectFolder / "Exports").string());
               o.baseName = str(a, "name", ctx.project.name);
               o.mp3.bitrateKbps = static_cast<int>(num(a, "bitrate", 320));
               o.mp3.vbr = str(a, "mp3Mode", "cbr") == "vbr";
               o.mp3.vbrQuality = static_cast<int>(num(a, "vbrQuality", 2));
               if (a.contains("metadata") && a["metadata"].is_object()) {
                   const json& m = a["metadata"];
                   o.mp3.title = m.value("title", ctx.project.name);
                   o.mp3.artist = m.value("artist", "");
                   o.mp3.album = m.value("album", "");
                   o.mp3.year = m.value("year", "");
                   o.mp3.comment = m.value("comment", "");
                   o.mp3.track = m.value("track", "");
                   o.mp3.genre = m.value("genre", "");
               } else {
                   o.mp3.title = ctx.project.name;
               }
               auto res = exporting::exportProject(ctx.runtime->engine(), *ctx.runtime, ctx.project, o);
               if (!res.ok) return fail(ctx, res.error);
               json files = json::array();
               for (auto& f : res.files)
                   files.push_back({{"path", f.path.string()}, {"what", f.what}, {"lufs", f.stats.integratedLufs}, {"truePeakDb", f.stats.truePeakDb},
                                    {"gainDb", f.appliedGainDb}});
               ctx.result = {{"files", files}, {"warnings", res.warnings}, {"seconds", res.renderedSeconds}};
               return true;
           }});
}

} // namespace roy
