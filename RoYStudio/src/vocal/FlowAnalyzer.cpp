#include "vocal/FlowAnalyzer.h"
#include "audio/ProjectRuntime.h"
#include "dsp/Analysis.h"

#include <algorithm>
#include <cmath>

namespace roy::vocal {

const char* placementName(Placement p) {
    switch (p) {
    case Placement::Early: return "EARLY";
    case Placement::OnBeat: return "ON BEAT";
    case Placement::Late: return "LATE";
    }
    return "?";
}

FlowReport analyzeFlow(const std::vector<std::vector<float>>& vocal, double sr, double startSec, const TempoMap& tempo,
                       const std::vector<double>& kicks, const std::vector<double>& snares, const FlowSettings& s) {
    FlowReport r;
    const auto x = dsp::mixToMono(vocal);
    dsp::OnsetSettings os;
    os.sensitivity = s.sensitivity;
    os.minGapSeconds = 0.06;
    auto onsets = dsp::detectOnsets(x.data(), static_cast<int64_t>(x.size()), sr, os);
    auto nearestMs = [&](const std::vector<double>& beats, double tSec) {
        double best = 1e9;
        for (double b : beats) {
            const double d = (tSec - tempo.beatToSeconds(b)) * 1000.0;
            if (std::fabs(d) < std::fabs(best)) best = d;
        }
        return best;
    };
    double sum = 0;
    int early = 0, on = 0, late = 0;
    for (auto& o : onsets) {
        FlowOnset f;
        f.timeSec = startSec + o.time;
        f.strength = o.strength;
        f.beat = tempo.secondsToBeat(f.timeSec);
        f.gridBeat = std::round(f.beat / s.gridBeats) * s.gridBeats;
        f.offsetMs = (f.timeSec - tempo.beatToSeconds(f.gridBeat)) * 1000.0;
        if (f.offsetMs < -s.onBeatToleranceMs) { f.placement = Placement::Early; ++early; }
        else if (f.offsetMs > s.onBeatToleranceMs) { f.placement = Placement::Late; ++late; }
        else { f.placement = Placement::OnBeat; ++on; }
        if (!kicks.empty()) f.kickOffsetMs = nearestMs(kicks, f.timeSec);
        if (!snares.empty()) f.snareOffsetMs = nearestMs(snares, f.timeSec);
        sum += f.offsetMs;
        r.onsets.push_back(f);
    }
    const double n = static_cast<double>(r.onsets.size());
    if (n > 0) {
        r.meanOffsetMs = sum / n;
        double var = 0;
        for (auto& f : r.onsets) var += (f.offsetMs - r.meanOffsetMs) * (f.offsetMs - r.meanOffsetMs);
        r.offsetStdMs = std::sqrt(var / n);
        r.earlyRatio = early / n;
        r.onRatio = on / n;
        r.lateRatio = late / n;
        r.onsetsPerSecond = n / std::max(1e-9, static_cast<double>(x.size()) / sr);
    }
    // pauses from the level envelope; phrases = activity between pauses
    auto fl = dsp::frameLevels(x.data(), static_cast<int64_t>(x.size()), sr, 0.02, 0.01);
    const double floor = dsp::percentile(fl.rmsDb, 0.1);
    const double peak = dsp::percentile(fl.rmsDb, 0.98);
    const double thr = std::max(floor + 10.0, peak - 30.0);
    std::vector<std::pair<double, double>> active;
    double runStart = -1;
    for (size_t i = 0; i <= fl.rmsDb.size(); ++i) {
        const bool act = i < fl.rmsDb.size() && fl.rmsDb[i] > thr;
        const double t = static_cast<double>(i) * fl.hopSeconds;
        if (act && runStart < 0) runStart = t;
        if (!act && runStart >= 0) {
            active.push_back({runStart, t});
            runStart = -1;
        }
    }
    // merge activity separated by less than the pause length
    std::vector<std::pair<double, double>> phrases;
    for (auto& a : active) {
        if (!phrases.empty() && a.first - phrases.back().second < s.pauseSeconds) phrases.back().second = a.second;
        else phrases.push_back(a);
    }
    for (size_t i = 1; i < phrases.size(); ++i) r.pauses.push_back({startSec + phrases[i - 1].second, startSec + phrases[i].first});
    for (size_t pi = 0; pi < phrases.size(); ++pi) {
        FlowPhrase ph;
        ph.startSec = startSec + phrases[pi].first;
        ph.endSec = startSec + phrases[pi].second;
        ph.startBeat = tempo.secondsToBeat(ph.startSec);
        ph.lengthBeats = tempo.secondsToBeat(ph.endSec) - ph.startBeat;
        double off = 0;
        for (auto& f : r.onsets)
            if (f.timeSec >= ph.startSec - 0.03 && f.timeSec < ph.endSec) {
                f.phrase = static_cast<int>(pi);
                ++ph.onsets;
                off += f.offsetMs;
            }
        ph.onsetsPerBeat = ph.lengthBeats > 0 ? ph.onsets / ph.lengthBeats : 0;
        ph.onsetsPerSecond = ph.endSec > ph.startSec ? ph.onsets / (ph.endSec - ph.startSec) : 0;
        ph.meanOffsetMs = ph.onsets ? off / ph.onsets : 0;
        r.phrases.push_back(ph);
    }
    return r;
}

std::vector<double> drumHitsFromProject(const Project& p, const std::string& voice, double from, double to) {
    std::vector<double> out;
    const int midiNote = voice == "kick" ? 36 : voice == "snare" ? 38 : voice == "clap" ? 39 : -1;
    for (auto& t : p.tracks) {
        for (auto& pc : t.patternClips) {
            const Pattern* pat = p.findPattern(pc.patternId);
            if (!pat || pc.muted) continue;
            Pattern filtered = *pat;
            for (auto& row : filtered.rows)
                if (row.voice != voice) row.muted = true;
            for (auto& n : expandPatternClip(filtered, pc, 1))
                if (n.beat >= from && n.beat < to) out.push_back(n.beat);
        }
        if (t.type == TrackType::Midi && midiNote >= 0)
            for (auto& c : t.midiClips)
                for (auto& n : c.notes)
                    if (n.pitch == midiNote && c.startBeat + n.startBeat >= from && c.startBeat + n.startBeat < to)
                        out.push_back(c.startBeat + n.startBeat);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace roy::vocal
