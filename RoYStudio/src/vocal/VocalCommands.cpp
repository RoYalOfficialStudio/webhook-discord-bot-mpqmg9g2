// Vocal Lab commands. Processing never modifies existing audio files: results
// are written to NEW files in <project>/Audio and the clip is re-pointed, so
// undo simply points the clip back to the original asset.
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "core/Math.h"
#include "io/AudioFile.h"
#include "project/ProjectIO.h"
#include "vocal/FlowAnalyzer.h"
#include "vocal/PitchGuardian.h"
#include "vocal/VocalDoctor.h"
#include "vocal/VocalTools.h"

#include <algorithm>
#include <cmath>
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

// The audio a clip plays, as a buffer aligned to the clip start (engine sample rate).
bool clipAudio(CommandContext& ctx, const AudioClip& c, vocal::Channels& out, double& sr) {
    if (!ctx.runtime) return fail(ctx, "audio runtime not available");
    if (std::fabs(c.stretch - 1.0) > 1e-6 || std::fabs(c.pitchSemitones) > 1e-6)
        return fail(ctx, "clip is stretched/transposed - render it first");
    auto data = ctx.runtime->asset(ctx.project, c.assetId);
    if (!data) return fail(ctx, "audio of the clip is not available");
    sr = data->sampleRate;
    const int64_t s0 = std::max<int64_t>(0, static_cast<int64_t>(std::llround(c.sourceOffsetSec * sr)));
    const double durSec = ctx.project.tempo.beatToSeconds(c.endBeat()) - ctx.project.tempo.beatToSeconds(c.startBeat);
    const int64_t len = static_cast<int64_t>(std::llround(durSec * sr));
    out.assign(static_cast<size_t>(data->numChannels), std::vector<float>(static_cast<size_t>(std::max<int64_t>(0, len)), 0.0f));
    for (int ch = 0; ch < data->numChannels; ++ch)
        for (int64_t i = 0; i < len; ++i) out[static_cast<size_t>(ch)][static_cast<size_t>(i)] = data->sample(ch, s0 + i);
    return true;
}

// Writes processed audio as a new asset and re-points the clip to it (whole clip span).
bool replaceClipAudio(CommandContext& ctx, AudioClip& c, const vocal::Channels& audio, double sr, const std::string& suffix) {
    if (ctx.projectFolder.empty()) return fail(ctx, "no project folder (save the project first)");
    const AudioAsset* src = ctx.project.findAsset(c.assetId);
    const std::string base = src ? fs::path(src->originalName).stem().string() : "clip";
    const fs::path file = files::uniquePath(ctx.projectFolder / "Audio" / std::format("{}_{}.wav", sanitizeFileName(base), suffix));
    std::string err;
    if (!writeWavFile(file, audio, sr, SampleFormat::Float32, false, &err)) return fail(ctx, "cannot write processed audio: " + err);
    AudioAsset a;
    a.id = files::newId();
    std::error_code ec;
    auto rel = fs::relative(file, ctx.projectFolder, ec);
    a.path = ec ? file.string() : rel.generic_string();
    a.originalName = file.filename().string();
    a.kind = "derived";
    a.sampleRate = sr;
    a.channels = static_cast<int>(audio.size());
    a.frames = audio.empty() ? 0 : static_cast<int64_t>(audio[0].size());
    a.sha256 = files::sha256File(file);
    a.createdAt = files::nowIso8601();
    ctx.project.assets.push_back(a);
    c.assetId = a.id;
    c.sourceOffsetSec = 0.0;
    ctx.result["assetId"] = a.id;
    ctx.result["file"] = file.string();
    return true;
}

vocal::PitchGuardianSettings guardianSettings(const Project& p, const json& a) {
    vocal::PitchGuardianSettings s;
    s.mode = vocal::guardianModeFromId(str(a, "mode", "assist"));
    s.key = p.key;
    if (auto k = parseKey(str(a, "key"))) s.key = *k;
    s.strength = num(a, "strength", s.strength);
    s.speedMs = num(a, "speedMs", s.speedMs);
    s.humanize = num(a, "humanize", s.humanize);
    s.formantPreserve = flag(a, "formantPreserve", s.formantPreserve);
    s.vibratoPreserve = num(a, "vibratoPreserve", s.vibratoPreserve);
    s.slidePreserve = num(a, "slidePreserve", s.slidePreserve);
    s.transitionMs = num(a, "transitionMs", s.transitionMs);
    s.scaleLock = flag(a, "scaleLock", s.scaleLock);
    s.offKeyFilter = flag(a, "offKeyFilter", s.offKeyFilter);
    s.allowChromatic = flag(a, "allowChromatic", s.allowChromatic);
    s.assistThresholdCents = num(a, "thresholdCents", s.assistThresholdCents);
    s.minConfidence = num(a, "minConfidence", s.minConfidence);
    return s;
}

json settingsJson(const vocal::PitchGuardianSettings& s) {
    return {{"mode", vocal::guardianModeId(s.mode)}, {"key", s.key.name()}, {"strength", s.strength}, {"speedMs", s.speedMs},
            {"humanize", s.humanize}, {"formantPreserve", s.formantPreserve}, {"vibratoPreserve", s.vibratoPreserve},
            {"slidePreserve", s.slidePreserve}, {"offKeyFilter", s.offKeyFilter}, {"allowChromatic", s.allowChromatic},
            {"thresholdCents", s.assistThresholdCents}, {"minConfidence", s.minConfidence}};
}

} // namespace

void registerVocalCommands(CommandRegistry& r) {
    r.add({"PitchGuardian", "Pitch Guardian", "Vocal", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* owner = nullptr;
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"), &owner);
               if (!c) return fail(ctx, "audio clip not found");
               if (c->locked) return fail(ctx, "clip is locked");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               auto s = guardianSettings(ctx.project, a);
               auto res = vocal::runPitchGuardian(audio, sr, s);
               json notes = json::array();
               int corrected = 0;
               for (auto& n : res.plan.notes) {
                   corrected += n.corrected ? 1 : 0;
                   notes.push_back({{"from", n.fromMidi}, {"to", n.toMidi}, {"shift", n.shift}, {"corrected", n.corrected}, {"reason", n.reason}});
               }
               json warnings = json::array();
               for (auto& w : res.plan.warnings) warnings.push_back(w.text);
               ctx.project.vocalSettings[owner->id]["pitchGuardian"] = settingsJson(s);
               ctx.result["notes"] = notes;
               ctx.result["warnings"] = warnings;
               ctx.result["corrected"] = corrected;
               if (s.mode == vocal::GuardianMode::Off || s.mode == vocal::GuardianMode::Warn || !res.plan.any()) return true; // analysis only
               return replaceClipAudio(ctx, *c, res.audio, sr, std::format("tuned_{}", vocal::guardianModeId(s.mode)));
           }});
    r.add({"PitchAnalysis", "Analyse Pitch", "Vocal", "", false, false, [](CommandContext& ctx, const json& a) {
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               auto track = vocal::detectPitch(audio, sr);
               auto an = vocal::analyzePitch(track);
               json notes = json::array();
               for (auto& n : an.notes)
                   notes.push_back({{"start", n.start}, {"end", n.end}, {"note", noteName(n.nearestNote)}, {"midi", n.medianMidi},
                                    {"cents", n.centsFromNearest}, {"confidence", n.meanConfidence}, {"kind", vocal::frameKindName(n.kind)},
                                    {"vibratoHz", n.vibratoRateHz}, {"vibratoCents", n.vibratoExtentCents}});
               ctx.result["notes"] = notes;
               ctx.result["inTuneRatio"] = an.inTuneRatio;
               ctx.result["rapIndicator"] = an.rapIndicator;
               json issues = json::array();
               for (auto& t : vocal::tuningIssues(an, ctx.project.key, num(a, "thresholdCents", 25.0))) issues.push_back(t.text);
               ctx.result["issues"] = issues;
               return true;
           }});
    r.add({"VocalDoctor", "Vocal Doctor", "Vocal", "", false, false, [](CommandContext& ctx, const json& a) {
               Track* owner = nullptr;
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"), &owner);
               if (!c) return fail(ctx, "audio clip not found");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               vocal::DoctorSettings ds;
               ds.key = ctx.project.key;
               ds.keyKnown = flag(a, "keyKnown", true);
               auto rep = vocal::examineVocal(audio, sr, ds);
               json j = rep.toJson();
               // Fill in the targets so each fix can be executed directly with its command.
               for (auto& issue : j["issues"])
                   for (auto& fx : issue["fixes"]) {
                       auto& args = fx["args"];
                       args["clipId"] = c->id;
                       args["trackId"] = owner->id;
                       args["channelId"] = owner->channelId;
                   }
               ctx.result = j;
               return true;
           }});
    r.add({"BreathReduce", "Reduce Breaths", "Vocal", "", true, true, [](CommandContext& ctx, const json& a) {
               Track* owner = nullptr;
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"), &owner);
               if (!c) return fail(ctx, "audio clip not found");
               MixerChannel* ch = ctx.project.findChannel(owner->channelId);
               if (!ch) return fail(ctx, "channel not found");
               const double red = num(a, "reductionDb", -8.0);
               const double base = ch->gainDb;
               AutomationLane* lane = nullptr;
               for (auto& l : ctx.project.automation)
                   if (l.channelId == ch->id && l.slotId.empty() && l.paramId == "gain") lane = &l;
               if (!lane) {
                   AutomationLane l;
                   l.id = files::newId();
                   l.channelId = ch->id;
                   l.paramId = "gain";
                   l.points.push_back({0.0, static_cast<float>(base)});
                   ctx.project.automation.push_back(l);
                   lane = &ctx.project.automation.back();
               }
               const double clipStartSec = ctx.project.tempo.beatToSeconds(c->startBeat);
               int count = 0;
               for (auto& reg : a.value("regions", json::array())) {
                   const double s0 = clipStartSec + num(reg, "start", 0.0), s1 = clipStartSec + num(reg, "end", 0.0);
                   if (s1 <= s0) continue;
                   const double ramp = std::min(0.02, (s1 - s0) / 4);
                   auto beat = [&](double sec) { return ctx.project.tempo.secondsToBeat(sec); };
                   lane->points.push_back({beat(s0 - ramp), static_cast<float>(base)});
                   lane->points.push_back({beat(s0 + ramp), static_cast<float>(base + red)});
                   lane->points.push_back({beat(s1 - ramp), static_cast<float>(base + red)});
                   lane->points.push_back({beat(s1 + ramp), static_cast<float>(base)});
                   ++count;
               }
               std::sort(lane->points.begin(), lane->points.end(), [](auto& x, auto& y) { return x.beat < y.beat; });
               ctx.result["regions"] = count;
               ctx.result["laneId"] = lane->id;
               return count > 0 || fail(ctx, "no breath regions given");
           }});
    r.add({"DoubleMagnet", "Double Magnet", "Vocal", "", true, true, [](CommandContext& ctx, const json& a) {
               AudioClip* mainC = ctx.project.findAudioClip(str(a, "mainClipId"));
               AudioClip* dblC = ctx.project.findAudioClip(str(a, "doubleClipId"));
               if (!mainC || !dblC) return fail(ctx, "main or double clip not found");
               if (dblC->locked) return fail(ctx, "double clip is locked");
               vocal::Channels mainA, dblA;
               double sr = 48000, sr2 = 48000;
               if (!clipAudio(ctx, *mainC, mainA, sr) || !clipAudio(ctx, *dblC, dblA, sr2)) return false;
               // Put the main vocal on the double clip's time base.
               const double offSec = ctx.project.tempo.beatToSeconds(mainC->startBeat) - ctx.project.tempo.beatToSeconds(dblC->startBeat);
               const int64_t shift = static_cast<int64_t>(std::llround(offSec * sr));
               vocal::Channels mainOnDbl(mainA.size(), std::vector<float>(dblA[0].size(), 0.0f));
               for (size_t ch = 0; ch < mainA.size(); ++ch)
                   for (int64_t i = 0; i < static_cast<int64_t>(dblA[0].size()); ++i) {
                       const int64_t src = i - shift;
                       if (src >= 0 && src < static_cast<int64_t>(mainA[ch].size())) mainOnDbl[ch][static_cast<size_t>(i)] = mainA[ch][static_cast<size_t>(src)];
                   }
               vocal::MagnetSettings s;
               s.mode = vocal::magnetModeFromId(str(a, "mode", "natural"));
               s.alignTiming = flag(a, "timing", true);
               s.alignPitch = flag(a, "pitch", false);
               s.pitchStrength = num(a, "pitchStrength", s.pitchStrength);
               auto res = vocal::alignDouble(mainOnDbl, dblA, sr, s);
               ctx.result["before_ms"] = res.report.meanAbsOffsetBeforeMs;
               ctx.result["after_ms"] = res.report.meanAbsOffsetAfterMs;
               ctx.result["pitch_before_cents"] = res.report.meanPitchDiffBeforeCents;
               ctx.result["pitch_after_cents"] = res.report.meanPitchDiffAfterCents;
               return replaceClipAudio(ctx, *dblC, res.audio, sr, std::format("magnet_{}", vocal::magnetModeId(s.mode)));
           }});
    r.add({"RegionGain", "Region Gain", "Vocal", "", true, true, [](CommandContext& ctx, const json& a) {
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               if (c->locked) return fail(ctx, "clip is locked");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               auto out = vocal::applyRegionGain(audio, sr, num(a, "start", 0.0), num(a, "end", 0.0), num(a, "gainDb", 0.0));
               return replaceClipAudio(ctx, *c, out, sr, "region_gain");
           }});
    r.add({"RegionPitch", "Region Pitch Shift", "Vocal", "", true, true, [](CommandContext& ctx, const json& a) {
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               if (c->locked) return fail(ctx, "clip is locked");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               auto out = vocal::applyRegionPitchShift(audio, sr, num(a, "start", 0.0), num(a, "end", 0.0), num(a, "semitones", 0.0),
                                                       flag(a, "formantPreserve", true));
               return replaceClipAudio(ctx, *c, out, sr, "region_pitch");
           }});
    r.add({"VocalMicroscope", "Vocal Microscope", "Vocal", "", false, false, [](CommandContext& ctx, const json& a) {
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               auto m = vocal::inspectRegion(audio, sr, num(a, "start", 0.0), num(a, "end", 1e9));
               ctx.result = {{"pitchMedian", m.pitchMedianMidi}, {"pitchMin", m.pitchMinMidi}, {"pitchMax", m.pitchMaxMidi},
                             {"pitchStabilityCents", m.pitchStabilityCents}, {"voicedRatio", m.voicedRatio}, {"onsets", m.onsets},
                             {"peakDb", m.peakDb}, {"rmsDb", m.rmsDb}, {"formantsHz", m.formantsHz},
                             {"sibilanceRatioDb", m.sibilanceRatioDb}, {"breath", m.looksLikeBreath}, {"noiseFloorDb", m.noiseFloorDb},
                             {"attackMs", m.attackMs}};
               return true;
           }});
    r.add({"FlowAnalyze", "Flow Analyzer", "Vocal", "", false, false, [](CommandContext& ctx, const json& a) {
               AudioClip* c = ctx.project.findAudioClip(str(a, "clipId"));
               if (!c) return fail(ctx, "audio clip not found");
               vocal::Channels audio;
               double sr = 48000;
               if (!clipAudio(ctx, *c, audio, sr)) return false;
               const double startSec = ctx.project.tempo.beatToSeconds(c->startBeat);
               auto kicks = vocal::drumHitsFromProject(ctx.project, "kick", c->startBeat - 1, c->endBeat() + 1);
               auto snares = vocal::drumHitsFromProject(ctx.project, "snare", c->startBeat - 1, c->endBeat() + 1);
               vocal::FlowSettings fs;
               fs.gridBeats = num(a, "gridBeats", fs.gridBeats);
               fs.onBeatToleranceMs = num(a, "toleranceMs", fs.onBeatToleranceMs);
               auto rep = vocal::analyzeFlow(audio, sr, startSec, ctx.project.tempo, kicks, snares, fs);
               json on = json::array();
               for (auto& o : rep.onsets)
                   on.push_back({{"time", o.timeSec}, {"beat", o.beat}, {"offsetMs", o.offsetMs}, {"placement", vocal::placementName(o.placement)},
                                 {"kickMs", o.kickOffsetMs}, {"snareMs", o.snareOffsetMs}, {"phrase", o.phrase}});
               json ph = json::array();
               for (auto& p : rep.phrases)
                   ph.push_back({{"startBeat", p.startBeat}, {"lengthBeats", p.lengthBeats}, {"onsets", p.onsets},
                                 {"onsetsPerBeat", p.onsetsPerBeat}, {"meanOffsetMs", p.meanOffsetMs}});
               ctx.result = {{"onsets", on}, {"phrases", ph}, {"meanOffsetMs", rep.meanOffsetMs}, {"offsetStdMs", rep.offsetStdMs},
                             {"early", rep.earlyRatio}, {"onBeat", rep.onRatio}, {"late", rep.lateRatio}, {"note", rep.note}};
               return true;
           }});
}

} // namespace roy
