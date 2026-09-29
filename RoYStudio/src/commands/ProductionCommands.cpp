// Production commands: import, sampler slicing, sample analysis, stems.
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "instruments/Sampler.h"
#include "io/AudioFile.h"
#include "midi/Scale.h"
#include "project/ProjectIO.h"
#include "sampler/SampleTools.h"
#include "stems/StemSeparator.h"

#include <format>

namespace roy {

namespace fs = std::filesystem;

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
bool flag(const json& a, const char* k, bool d) {
    auto it = a.find(k);
    return it != a.end() && it->is_boolean() ? it->get<bool>() : d;
}

AudioAsset assetForFile(const fs::path& file, const fs::path& folder, const std::string& kind) {
    AudioAsset a;
    AudioFileInfo info;
    probeAudioFile(file, info);
    a.id = files::newId();
    std::error_code ec;
    auto rel = fs::relative(file, folder, ec);
    a.path = (ec || rel.empty() || rel.string().rfind("..", 0) == 0) ? file.string() : rel.generic_string();
    a.originalName = file.filename().string();
    a.kind = kind;
    a.sampleRate = info.sampleRate;
    a.channels = info.channels;
    a.frames = info.frames;
    a.sha256 = files::sha256File(file);
    a.createdAt = files::nowIso8601();
    return a;
}

bool assetChannels(CommandContext& ctx, const std::string& assetId, stems::Channels& out, double& sr) {
    if (!ctx.runtime) return fail(ctx, "audio runtime not available");
    auto d = ctx.runtime->asset(ctx.project, assetId);
    if (!d) return fail(ctx, "asset not available");
    out = d->channels;
    sr = d->sampleRate;
    return true;
}
} // namespace

// Imports an audio file (copied into the project) and, with "trackId", places it as a clip.
static bool importAudioFile(CommandContext& ctx, const json& a) {
    const fs::path src = str(a, "path");
    AudioFileInfo info;
    std::string err;
    if (!probeAudioFile(src, info, &err)) return fail(ctx, "cannot read audio file: " + err);
    fs::path file = src;
    if (flag(a, "copy", true) && !ctx.projectFolder.empty()) {
        // Copy into the project (never move, never overwrite).
        file = files::uniquePath(ctx.projectFolder / "Audio" / src.filename());
        if (!files::safeCopy(src, file, &err)) return fail(ctx, "copy failed: " + err);
    }
    AudioAsset asset = assetForFile(file, ctx.projectFolder, "import");
    ctx.project.assets.push_back(asset);
    ctx.result["assetId"] = asset.id;
    if (!str(a, "trackId").empty()) {
        Track* t = ctx.project.findTrack(str(a, "trackId"));
        if (!t || t->type != TrackType::Audio) return fail(ctx, "audio track not found");
        AudioClip c;
        c.id = files::newId();
        c.assetId = asset.id;
        c.name = src.stem().string();
        c.startBeat = std::max(0.0, num(a, "startBeat", 0.0));
        const double secs = info.sampleRate > 0 ? static_cast<double>(info.frames) / info.sampleRate : 1.0;
        c.lengthBeats = ctx.project.tempo.secondsToBeat(ctx.project.tempo.beatToSeconds(c.startBeat) + secs) - c.startBeat;
        t->audioClips.push_back(c);
        ctx.result["clipId"] = c.id;
    }
    return true;
}

void registerProductionCommands(CommandRegistry& r) {
    r.add({"ImportAudio", "Import Audio File", "Browser", "Ctrl+I", true, true, [](CommandContext& ctx, const json& a) {
               return importAudioFile(ctx, a);
           }});
    // IMPORT BEAT: a finished beat (MP3/WAV/...) on its own new audio track, optionally with the
    // song tempo and key taken over - all in ONE undo step.
    r.add({"ImportBeat", "Import Beat", "Browser", "", true, true, [](CommandContext& ctx, const json& a) {
               const fs::path src = str(a, "path");
               AudioFileInfo info;
               std::string err;
               if (!probeAudioFile(src, info, &err)) return fail(ctx, "cannot read audio file: " + err);
               // tempo first: the clip length in beats is computed with the new tempo
               if (a.contains("bpm")) {
                   const double bpm = num(a, "bpm", 0.0);
                   if (bpm < 10 || bpm > 999) return fail(ctx, "tempo must be between 10 and 999 BPM");
                   ctx.project.tempo.setTempo(bpm);
               }
               if (!str(a, "key").empty()) {
                   auto k = parseKey(str(a, "key"));
                   if (!k) return fail(ctx, "cannot parse key " + str(a, "key"));
                   ctx.project.key = *k;
               }
               std::string trackId = str(a, "trackId");
               if (trackId.empty()) {
                   Track& t = addTrack(ctx.project, TrackType::Audio, str(a, "name", src.stem().string()));
                   t.role = "beat";
                   trackId = t.id;
               }
               json b = a;
               b["trackId"] = trackId;
               if (!importAudioFile(ctx, b)) return false;
               ctx.result["trackId"] = trackId;
               ctx.result["seconds"] = info.sampleRate > 0 ? static_cast<double>(info.frames) / info.sampleRate : 0.0;
               return true;
           }});
    r.add({"AnalyzeSample", "Analyze Sample", "Browser", "", false, false, [](CommandContext& ctx, const json& a) {
               stems::Channels ch;
               double sr = 48000;
               if (!assetChannels(ctx, str(a, "assetId"), ch, sr)) return false;
               auto i = sampler::analyzeSample(ch, sr);
               ctx.result = {{"durationSec", i.durationSec}, {"peakDb", i.peakDb}, {"bpm", i.bpm}, {"bpmConfidence", i.bpmConfidence},
                             {"key", i.keyRoot >= 0 ? std::format("{} {}", pitchClassName(i.keyRoot), i.keyMinor ? "Minor" : "Major") : ""},
                             {"keyConfidence", i.keyConfidence}, {"rootNote", i.root.note >= 0 ? noteName(i.root.note) : ""},
                             {"rootCents", i.root.cents}, {"transients", i.transients}, {"loop", i.looksLikeLoop}};
               return true;
           }});
    r.add({"SliceToPads", "Slice To Pads", "Sampler", "", true, true, [](CommandContext& ctx, const json& a) {
               const std::string assetId = str(a, "assetId");
               stems::Channels ch;
               double sr = 48000;
               if (!assetChannels(ctx, assetId, ch, sr)) return false;
               std::vector<std::pair<int64_t, int64_t>> slices;
               if (str(a, "mode", "transients") == "grid")
                   slices = sampler::slicesGrid(ch.empty() ? 0 : static_cast<int64_t>(ch[0].size()), static_cast<int>(num(a, "count", 16)));
               else
                   slices = sampler::slicesFromTransients(ch, sr, num(a, "sensitivity", 1.0));
               if (slices.empty()) return fail(ctx, "no slices found");
               const int first = static_cast<int>(num(a, "firstNote", 36));
               auto zones = sampler::sliceToPads(assetId, slices, first, flag(a, "choke", false));
               Track& t = addTrack(ctx.project, TrackType::Midi, str(a, "name", "Sliced"));
               PluginSlot slot;
               slot.id = files::newId();
               slot.typeId = "roy.sampler";
               slot.name = "RoY Sampler";
               slot.state = json::object();
               slot.state["zones"] = json::array();
               for (auto& z : zones) slot.state["zones"].push_back(zoneToJson(z));
               t.instrument = slot;
               // A clip that re-plays the slices in their original rhythm.
               if (flag(a, "createClip", true)) {
                   MidiClip c;
                   c.id = files::newId();
                   c.name = "Slices";
                   for (size_t i = 0; i < slices.size(); ++i) {
                       MidiNote n;
                       n.pitch = first + static_cast<int>(i);
                       n.startBeat = ctx.project.tempo.secondsToBeat(static_cast<double>(slices[i].first) / sr);
                       n.lengthBeats = std::max(0.01, ctx.project.tempo.secondsToBeat(static_cast<double>(slices[i].second) / sr) - n.startBeat);
                       c.notes.push_back(n);
                   }
                   c.lengthBeats = std::max(1.0, ctx.project.tempo.secondsToBeat(static_cast<double>(slices.back().second) / sr));
                   t.midiClips.push_back(c);
               }
               ctx.result["trackId"] = t.id;
               ctx.result["slices"] = slices.size();
               return true;
           }});
    r.add({"SeparateStems", "Separate Stems", "Stems", "", true, true, [](CommandContext& ctx, const json& a) {
               stems::registerBuiltinSeparators();
               auto* sep = stems::StemRegistry::instance().find(str(a, "separator", "roy.dsp-basic"));
               if (!sep) return fail(ctx, "stem separator not found");
               std::string why;
               if (!sep->available(&why)) return fail(ctx, "separator not available: " + why);
               if (ctx.projectFolder.empty()) return fail(ctx, "no project folder");
               const std::string assetId = str(a, "assetId");
               stems::Channels ch;
               double sr = 48000;
               if (!assetChannels(ctx, assetId, ch, sr)) return false;
               auto res = sep->separate(ch, sr);
               if (res.stems.empty()) return fail(ctx, "separation cancelled");
               const AudioAsset* src = ctx.project.findAsset(assetId);
               std::string err;
               auto filesOut = stems::writeStems(res, sr, ctx.projectFolder / "Stems",
                                                 sanitizeFileName(src ? fs::path(src->originalName).stem().string() : "stems"), &err);
               if (filesOut.empty()) return fail(ctx, "writing stems failed: " + err);
               json created = json::array();
               const double startBeat = num(a, "startBeat", 0.0);
               for (size_t i = 0; i < filesOut.size(); ++i) {
                   AudioAsset as = assetForFile(filesOut[i], ctx.projectFolder, "stem");
                   ctx.project.assets.push_back(as);
                   if (flag(a, "createTracks", true)) {
                       Track& t = addTrack(ctx.project, TrackType::Audio, res.names[i]);
                       AudioClip c;
                       c.id = files::newId();
                       c.assetId = as.id;
                       c.name = res.names[i];
                       c.startBeat = startBeat;
                       c.lengthBeats = ctx.project.tempo.secondsToBeat(ctx.project.tempo.beatToSeconds(startBeat) + static_cast<double>(as.frames) / sr) - startBeat;
                       t.audioClips.push_back(c);
                       created.push_back({{"stem", res.names[i]}, {"assetId", as.id}, {"trackId", t.id}});
                   } else {
                       created.push_back({{"stem", res.names[i]}, {"assetId", as.id}});
                   }
               }
               ctx.result["stems"] = created;
               ctx.result["quality"] = {{"method", res.quality.method}, {"reconstructionErrorDb", res.quality.reconstructionErrorDb},
                                        {"crossTalk", res.quality.crossTalk}, {"warnings", res.quality.warnings}};
               return true;
           }});
}

} // namespace roy
