#include "intelligence/Assistant.h"
#include "audio/Processor.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "plugins/Sandbox.h"
#include "vocal/PitchAnalysis.h"
#include "vocal/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace roy::assist {

namespace fs = std::filesystem;

nlohmann::json VocalDna::toJson() const {
    return {{"takes", takes}, {"seconds", seconds}, {"pitchLowMidi", pitchLowMidi}, {"pitchMedianMidi", pitchMedianMidi},
            {"pitchHighMidi", pitchHighMidi}, {"dynamicRangeDb", dynamicRangeDb}, {"vibratoRateHz", vibratoRateHz},
            {"vibratoExtentCents", vibratoExtentCents}, {"meanPhraseSeconds", meanPhraseSeconds}, {"inTuneRatio", inTuneRatio},
            {"sustainedRatio", sustainedRatio},
            {"note", "Local profile of the user's own voice for analysis/editing only - not for voice imitation."}};
}

VocalDna VocalDna::fromJson(const nlohmann::json& j) {
    VocalDna d;
    d.takes = j.value("takes", 0);
    d.seconds = j.value("seconds", 0.0);
    d.pitchLowMidi = j.value("pitchLowMidi", 0.0);
    d.pitchMedianMidi = j.value("pitchMedianMidi", 0.0);
    d.pitchHighMidi = j.value("pitchHighMidi", 0.0);
    d.dynamicRangeDb = j.value("dynamicRangeDb", 0.0);
    d.vibratoRateHz = j.value("vibratoRateHz", 0.0);
    d.vibratoExtentCents = j.value("vibratoExtentCents", 0.0);
    d.meanPhraseSeconds = j.value("meanPhraseSeconds", 0.0);
    d.inTuneRatio = j.value("inTuneRatio", 0.0);
    d.sustainedRatio = j.value("sustainedRatio", 0.0);
    return d;
}

namespace {
struct TakeStats {
    double secs = 0, low = 0, med = 0, high = 0, dyn = 0, vibRate = 0, vibExt = 0, phrase = 0, inTune = 0, sustained = 0;
    bool voiced = false;
};

TakeStats measure(const std::vector<std::vector<float>>& audio, double sr) {
    TakeStats s;
    const auto x = dsp::mixToMono(audio);
    s.secs = static_cast<double>(x.size()) / sr;
    auto track = vocal::detectPitch(x.data(), static_cast<int64_t>(x.size()), sr);
    auto a = vocal::analyzePitch(track);
    std::vector<double> v;
    for (auto& f : track.frames)
        if (f.voiced) v.push_back(f.midi);
    if (v.size() < 10) return s;
    s.voiced = true;
    std::sort(v.begin(), v.end());
    s.low = v[v.size() / 20];
    s.med = v[v.size() / 2];
    s.high = v[v.size() * 19 / 20];
    auto fl = dsp::frameLevels(x.data(), static_cast<int64_t>(x.size()), sr, 0.05, 0.025);
    std::vector<float> act;
    for (float d : fl.rmsDb)
        if (d > -50.0f) act.push_back(d);
    s.dyn = act.size() > 4 ? dsp::percentile(act, 0.95) - dsp::percentile(act, 0.1) : 0.0;
    int vn = 0;
    for (auto& nt : a.notes)
        if (nt.kind == vocal::FrameKind::Vibrato) {
            s.vibRate += nt.vibratoRateHz;
            s.vibExt += nt.vibratoExtentCents;
            ++vn;
        }
    if (vn) {
        s.vibRate /= vn;
        s.vibExt /= vn;
    }
    // phrases = voiced notes merged across gaps < 0.3 s
    std::vector<std::pair<double, double>> ph;
    for (auto& nt : a.notes) {
        if (!ph.empty() && nt.start - ph.back().second < 0.3) ph.back().second = nt.end;
        else ph.push_back({nt.start, nt.end});
    }
    double sum = 0;
    for (auto& [b, e] : ph) sum += e - b;
    s.phrase = ph.empty() ? 0 : sum / static_cast<double>(ph.size());
    s.inTune = a.inTuneRatio;
    s.sustained = a.sustainedRatio;
    return s;
}
} // namespace

void learnVocal(VocalDna& d, const std::vector<std::vector<float>>& audio, double sr) {
    const TakeStats s = measure(audio, sr);
    if (!s.voiced) return;
    const double w0 = d.seconds, w1 = s.secs, W = w0 + w1;
    auto mix = [&](double a, double b) { return W > 0 ? (a * w0 + b * w1) / W : b; };
    d.pitchLowMidi = d.takes ? std::min(d.pitchLowMidi, s.low) : s.low;
    d.pitchHighMidi = d.takes ? std::max(d.pitchHighMidi, s.high) : s.high;
    d.pitchMedianMidi = mix(d.pitchMedianMidi, s.med);
    d.dynamicRangeDb = mix(d.dynamicRangeDb, s.dyn);
    if (s.vibRate > 0) {
        d.vibratoRateHz = d.vibratoRateHz > 0 ? mix(d.vibratoRateHz, s.vibRate) : s.vibRate;
        d.vibratoExtentCents = d.vibratoExtentCents > 0 ? mix(d.vibratoExtentCents, s.vibExt) : s.vibExt;
    }
    d.meanPhraseSeconds = mix(d.meanPhraseSeconds, s.phrase);
    d.inTuneRatio = mix(d.inTuneRatio, s.inTune);
    d.sustainedRatio = mix(d.sustainedRatio, s.sustained);
    d.seconds = W;
    ++d.takes;
}

std::vector<DnaDeviation> compareToDna(const VocalDna& d, const std::vector<std::vector<float>>& take, double sr) {
    std::vector<DnaDeviation> out;
    if (d.takes == 0) return out;
    const TakeStats s = measure(take, sr);
    if (!s.voiced) return out;
    if (s.high > d.pitchHighMidi + 1.0)
        out.push_back({"pitch_high", std::format("reaches {} - {:.1f} semitones above your usual top note", noteName(static_cast<int>(std::lround(s.high))), s.high - d.pitchHighMidi), s.high, d.pitchHighMidi});
    if (s.low < d.pitchLowMidi - 1.0)
        out.push_back({"pitch_low", std::format("goes down to {} - below your usual range", noteName(static_cast<int>(std::lround(s.low)))), s.low, d.pitchLowMidi});
    if (std::fabs(s.med - d.pitchMedianMidi) > 2.0)
        out.push_back({"pitch_centre", std::format("pitch centre {:.1f} st from your usual centre", s.med - d.pitchMedianMidi), s.med, d.pitchMedianMidi});
    if (d.vibratoRateHz > 0 && s.vibRate > 0 && std::fabs(s.vibRate - d.vibratoRateHz) > 1.0)
        out.push_back({"vibrato_rate", std::format("vibrato {:.1f} Hz vs your usual {:.1f} Hz", s.vibRate, d.vibratoRateHz), s.vibRate, d.vibratoRateHz});
    if (std::fabs(s.inTune - d.inTuneRatio) > 0.2)
        out.push_back({"tuning", std::format("{:.0f} % in tune vs your usual {:.0f} %", s.inTune * 100, d.inTuneRatio * 100), s.inTune, d.inTuneRatio});
    if (d.dynamicRangeDb > 0 && std::fabs(s.dyn - d.dynamicRangeDb) > 6.0)
        out.push_back({"dynamics", std::format("dynamic range {:.0f} dB vs your usual {:.0f} dB", s.dyn, d.dynamicRangeDb), s.dyn, d.dynamicRangeDb});
    return out;
}

// ---------------------------------------------------------------- project assistant
namespace {
// Built-in types must be registered; plugin types (clap:/vst3:) must point to an existing module.
bool processorAvailable(const std::string& typeId) {
    std::string format, module, id;
    if (plugins::parsePluginTypeId(typeId, format, module, id)) {
        std::error_code ec;
        return fs::exists(module, ec);
    }
    if (typeId.rfind("clap:", 0) == 0 || typeId.rfind("vst3:", 0) == 0) return false; // malformed plugin id
    return ProcessorFactory::instance().has(typeId);
}
} // namespace

std::vector<Finding> checkProject(const Project& p, const fs::path& folder, bool dirty, size_t backups) {
    std::vector<Finding> out;
    std::set<std::string> used;
    for (auto& t : p.tracks) {
        for (auto& c : t.audioClips) {
            used.insert(c.assetId);
            if (!c.rawAssetId.empty()) used.insert(c.rawAssetId);     // vocal A/B: original ...
            if (!c.tunedAssetId.empty()) used.insert(c.tunedAssetId); // ... and tuned render
        }
        for (auto& k : t.takes) used.insert(k.assetId);
        for (auto& c : t.patternClips)
            if (const Pattern* pat = p.findPattern(c.patternId))
                for (auto& r : pat->rows)
                    if (!r.sampleAssetId.empty()) used.insert(r.sampleAssetId);
        if (t.instrument && t.instrument->state.contains("zones"))
            for (auto& z : t.instrument->state["zones"])
                if (z.contains("assetId")) used.insert(z["assetId"].get<std::string>());
    }
    std::error_code ec;
    for (auto& a : p.assets) {
        fs::path f(a.path);
        if (!f.is_absolute()) f = folder / f;
        if (a.path.rfind("memory://", 0) != 0 && !fs::exists(f, ec))
            out.push_back({"missing_file", "problem", std::format("audio file missing: {} ({})", a.originalName, a.path), "", {}});
        if (!used.count(a.id))
            out.push_back({"unused_asset", "info", std::format("{} is not used by any clip (the file is kept)", a.originalName), "", {}});
        if (a.sampleRate > 0 && std::fabs(a.sampleRate - p.sampleRate) > 0.5)
            out.push_back({"sample_rate", "info", std::format("{} is {:.0f} Hz (project {:.0f} Hz) - resampled on playback", a.originalName, a.sampleRate, p.sampleRate), "", {}});
    }
    bool anySolo = false;
    for (auto& c : p.channels) anySolo |= c.solo;
    if (anySolo) out.push_back({"solo_active", "warning", "solo is active - other tracks are muted in playback and export", "", {}});
    for (auto& t : p.tracks) {
        const bool empty = t.audioClips.empty() && t.midiClips.empty() && t.patternClips.empty() && t.comp.empty();
        if (empty) out.push_back({"empty_track", "info", std::format("track '{}' has no clips", t.name), "DeleteTrack", {{"trackId", t.id}}});
        if (t.armed) out.push_back({"armed", "info", std::format("track '{}' is armed for recording", t.name), "ArmTrack", {{"trackId", t.id}, {"armed", false}}});
        if (const MixerChannel* ch = p.findChannel(t.channelId)) {
            if (ch->mute && !empty) out.push_back({"muted", "info", std::format("track '{}' is muted", t.name), "MuteChannel", {{"channelId", ch->id}, {"mute", false}}});
            if (!ch->outputChannelId.empty() && !p.findChannel(ch->outputChannelId))
                out.push_back({"bad_route", "problem", std::format("track '{}' routes to a missing bus - it plays through the master", t.name), "RouteChannel", {{"channelId", ch->id}, {"output", ""}}});
        }
        if (t.instrument && !processorAvailable(t.instrument->typeId))
            out.push_back({"missing_plugin", "problem", std::format("instrument '{}' on '{}' is not available", t.instrument->typeId, t.name), "", {}});
    }
    for (auto& c : p.channels)
        for (auto& s : c.inserts)
            if (!processorAvailable(s.typeId))
                out.push_back({"missing_plugin", "problem", std::format("effect '{}' on {} is not available (bypassed)", s.typeId, c.name), "", {}});
    for (auto& a : p.automation)
        if (!p.findChannel(a.channelId)) out.push_back({"dead_automation", "info", "automation lane points to a deleted channel", "DeleteAutomation", {{"laneId", a.id}}});
    bool hasLimiter = false;
    if (const MixerChannel* m = p.master())
        for (auto& s : m->inserts) hasLimiter |= s.typeId == "roy.limiter" && !s.bypass;
    if (!hasLimiter && !p.tracks.empty())
        out.push_back({"no_master_limiter", "info", "no limiter on the master - exports can clip inter-sample peaks", "CreateMasterChain", {{"preset", "streaming"}}});
    if (dirty) out.push_back({"unsaved", "warning", "unsaved changes", "", {}});
    if (backups == 0 && !folder.empty()) out.push_back({"no_backup", "info", "no numbered backup yet (created on the next save)", "", {}});
    return out;
}

} // namespace roy::assist
