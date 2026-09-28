#include "record/Takes.h"
#include "core/Files.h"

#include <algorithm>
#include <format>

namespace roy::takes {

namespace fs = std::filesystem;

std::vector<RecordTrackConfig> recordConfig(const Project& p) {
    std::vector<RecordTrackConfig> out;
    for (auto& t : p.tracks)
        if (t.armed && t.type == TrackType::Audio && t.inputLeft >= 0)
            out.push_back({t.id, t.name, t.inputLeft, t.inputRight, t.inputGainDb});
    return out;
}

namespace {
std::string relativeToProject(const fs::path& file, const fs::path& folder) {
    std::error_code ec;
    auto rel = fs::relative(file, folder, ec);
    if (ec || rel.empty() || rel.string().rfind("..", 0) == 0) return file.string();
    return rel.generic_string();
}

AudioAsset makeAsset(const fs::path& path, const fs::path& folder, const std::string& kind, double sr, int ch, int64_t frames) {
    AudioAsset a;
    a.id = files::newId();
    a.path = relativeToProject(path, folder);
    a.originalName = path.filename().string();
    a.kind = kind;
    a.sampleRate = sr;
    a.channels = ch;
    a.frames = frames;
    a.sha256 = files::sha256File(path);
    a.createdAt = files::nowIso8601();
    return a;
}
} // namespace

std::string addRecordedTake(Project& p, const FinishedTake& ft, const fs::path& folder) {
    Track* t = p.findTrack(ft.trackId);
    if (!t) return {};
    AudioAsset a = makeAsset(ft.path, folder, "recording", ft.sampleRate, ft.channels, ft.frames);
    p.assets.push_back(a);
    Take k;
    k.id = files::newId();
    k.assetId = a.id;
    int lane = 0;
    for (auto& x : t->takes) lane = std::max(lane, x.lane + 1);
    k.lane = lane;
    k.name = std::format("Take {}", t->takes.size() + 1);
    const double startSec = static_cast<double>(ft.timelineStart) / ft.sampleRate;
    const double endSec = static_cast<double>(ft.timelineStart + ft.frames) / ft.sampleRate;
    k.startBeat = p.tempo.secondsToBeat(startSec);
    k.lengthBeats = p.tempo.secondsToBeat(endSec) - k.startBeat;
    k.createdAt = a.createdAt;
    k.loopPass = ft.loopPass;
    t->takes.push_back(k);
    // If recording began before zero (pre-roll + latency) only the part >= 0 is comped.
    compSelect(*t, k.id, std::max(0.0, k.startBeat), k.startBeat + k.lengthBeats);
    return k.id;
}

bool compSelect(Track& t, const std::string& takeId, double s, double e) {
    auto tk = std::find_if(t.takes.begin(), t.takes.end(), [&](auto& x) { return x.id == takeId; });
    if (tk == t.takes.end() || e <= s) return false;
    // A comp segment can only use material the take actually contains.
    s = std::max(s, tk->startBeat);
    e = std::min(e, tk->startBeat + tk->lengthBeats);
    if (e <= s) return false;
    std::vector<CompSegment> out;
    for (auto& seg : t.comp) {
        if (seg.endBeat <= s || seg.startBeat >= e) {
            out.push_back(seg);
            continue;
        }
        if (seg.startBeat < s) out.push_back({seg.startBeat, s, seg.takeId});
        if (seg.endBeat > e) out.push_back({e, seg.endBeat, seg.takeId});
    }
    out.push_back({s, e, takeId});
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.startBeat < b.startBeat; });
    // merge adjacent segments of the same take
    std::vector<CompSegment> merged;
    for (auto& seg : out) {
        if (!merged.empty() && merged.back().takeId == seg.takeId && std::fabs(merged.back().endBeat - seg.startBeat) < 1e-9)
            merged.back().endBeat = seg.endBeat;
        else
            merged.push_back(seg);
    }
    t.comp = merged;
    return true;
}

bool compWholeTake(Track& t, const std::string& takeId) {
    auto tk = std::find_if(t.takes.begin(), t.takes.end(), [&](auto& x) { return x.id == takeId; });
    if (tk == t.takes.end()) return false;
    return compSelect(t, takeId, tk->startBeat, tk->startBeat + tk->lengthBeats);
}

std::vector<std::string> flattenComp(Project& p, const std::string& trackId) {
    std::vector<std::string> ids;
    Track* t = p.findTrack(trackId);
    if (!t) return ids;
    for (auto& seg : t->comp) {
        auto tk = std::find_if(t->takes.begin(), t->takes.end(), [&](auto& x) { return x.id == seg.takeId; });
        if (tk == t->takes.end()) continue;
        AudioClip c;
        c.id = files::newId();
        c.assetId = tk->assetId;
        c.name = tk->name;
        c.startBeat = seg.startBeat;
        c.lengthBeats = seg.endBeat - seg.startBeat;
        c.sourceOffsetSec = p.tempo.beatToSeconds(seg.startBeat) - p.tempo.beatToSeconds(tk->startBeat);
        c.fadeInBeats = c.fadeOutBeats = std::min(0.02, c.lengthBeats / 4);
        t->audioClips.push_back(c);
        ids.push_back(c.id);
    }
    t->comp.clear();
    return ids;
}

bool removeTake(Track& t, const std::string& takeId) {
    const auto n = std::erase_if(t.takes, [&](auto& x) { return x.id == takeId; });
    std::erase_if(t.comp, [&](auto& s) { return s.takeId == takeId; });
    return n > 0;
}

std::string addRecoveredPerformance(Project& p, const RecoveredPerformance& r, const fs::path& folder, double fallbackBeat) {
    Track* t = p.findTrack(r.trackId);
    if (!t) return {};
    AudioAsset a = makeAsset(r.path, folder, "recovery", p.sampleRate, r.channels, r.frames);
    p.assets.push_back(a);
    AudioClip c;
    c.id = files::newId();
    c.assetId = a.id;
    c.name = "Recovered Performance";
    c.color = 0x32CD32;
    double startSec = r.hasTimeline ? static_cast<double>(r.timelineStart) / p.sampleRate : p.tempo.beatToSeconds(fallbackBeat);
    if (startSec < 0) {
        c.sourceOffsetSec = -startSec;
        startSec = 0;
    }
    c.startBeat = p.tempo.secondsToBeat(startSec);
    const double endSec = startSec + static_cast<double>(r.frames) / p.sampleRate - c.sourceOffsetSec;
    c.lengthBeats = p.tempo.secondsToBeat(endSec) - c.startBeat;
    t->audioClips.push_back(c);
    return c.id;
}

} // namespace roy::takes
