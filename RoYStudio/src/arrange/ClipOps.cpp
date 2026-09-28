#include "arrange/ClipOps.h"
#include "core/Files.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::arrange {

namespace {
constexpr double kEps = 1e-9;
const std::string kEmpty;
double secs(const Project& p, double beat) { return p.tempo.beatToSeconds(beat); }
} // namespace

double snap(double beat, double gridBeats) {
    if (gridBeats <= 0) return beat;
    return std::round(beat / gridBeats) * gridBeats;
}

std::string formatTime(const Project& p, double beat, TimeFormat f) {
    switch (f) {
    case TimeFormat::BarsBeats: {
        const auto bb = p.tempo.beatToBarBeat(beat);
        const int beatIdx = static_cast<int>(std::floor(bb.beatInBar + 1e-9));
        const int ticks = static_cast<int>(std::floor((bb.beatInBar - beatIdx) * 960.0 + 1e-6));
        return std::format("{}.{}.{:03d}", bb.bar + 1, beatIdx + 1, ticks);
    }
    case TimeFormat::Seconds: {
        const double s = secs(p, beat);
        const int m = static_cast<int>(s / 60.0);
        return std::format("{}:{:06.3f}", m, s - m * 60.0);
    }
    case TimeFormat::Samples: return std::format("{}", std::llround(secs(p, beat) * p.sampleRate));
    }
    return {};
}

double& ClipRef::start() { return audio ? audio->startBeat : midi ? midi->startBeat : pattern->startBeat; }
double& ClipRef::length() { return audio ? audio->lengthBeats : midi ? midi->lengthBeats : pattern->lengthBeats; }
bool ClipRef::locked() const {
    return (audio && audio->locked) || (midi && midi->locked) || (pattern && pattern->locked) || (track && track->locked);
}
const std::string& ClipRef::groupId() const { return audio ? audio->groupId : midi ? midi->groupId : kEmpty; }
const std::string& ClipRef::id() const { return audio ? audio->id : midi ? midi->id : pattern->id; }

ClipRef findClip(Project& p, const std::string& clipId) {
    ClipRef r;
    for (auto& t : p.tracks) {
        for (auto& c : t.audioClips)
            if (c.id == clipId) { r.track = &t; r.audio = &c; return r; }
        for (auto& c : t.midiClips)
            if (c.id == clipId) { r.track = &t; r.midi = &c; return r; }
        for (auto& c : t.patternClips)
            if (c.id == clipId) { r.track = &t; r.pattern = &c; return r; }
    }
    return r;
}

double maxAudioClipLengthBeats(const Project& p, const AudioClip& c) {
    const AudioAsset* a = p.findAsset(c.assetId);
    if (!a || a->sampleRate <= 0 || a->frames <= 0) return 1e9; // unknown: do not clamp
    const double srcSeconds = static_cast<double>(a->frames) / a->sampleRate - c.sourceOffsetSec;
    const double endSec = secs(p, c.startBeat) + std::max(0.0, srcSeconds) * c.stretch;
    return p.tempo.secondsToBeat(endSec) - c.startBeat;
}

EditResult moveClip(Project& p, const std::string& clipId, double newStartBeat, const std::string& targetTrackId, bool moveGroup) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    newStartBeat = std::max(0.0, newStartBeat);
    const double delta = newStartBeat - c.start();
    // Group members move together (same delta), only within their own tracks.
    if (moveGroup && !c.groupId().empty()) {
        const std::string g = c.groupId();
        for (auto& t : p.tracks) {
            for (auto& a : t.audioClips)
                if (a.groupId == g && a.id != clipId && (a.locked || t.locked)) return EditResult::fail("group contains a locked clip");
            for (auto& m : t.midiClips)
                if (m.groupId == g && m.id != clipId && (m.locked || t.locked)) return EditResult::fail("group contains a locked clip");
        }
        for (auto& t : p.tracks) {
            for (auto& a : t.audioClips)
                if (a.groupId == g && a.id != clipId) a.startBeat = std::max(0.0, a.startBeat + delta);
            for (auto& m : t.midiClips)
                if (m.groupId == g && m.id != clipId) m.startBeat = std::max(0.0, m.startBeat + delta);
        }
    }
    c.start() = newStartBeat;
    if (!targetTrackId.empty() && targetTrackId != c.track->id) {
        Track* dst = p.findTrack(targetTrackId);
        if (!dst) return EditResult::fail("target track not found");
        if (dst->locked) return EditResult::fail("target track is locked");
        Track* src = c.track;
        if (c.audio) {
            if (dst->type != TrackType::Audio) return EditResult::fail("audio clips can only move to audio tracks");
            dst->audioClips.push_back(*c.audio);
            std::erase_if(src->audioClips, [&](auto& x) { return x.id == clipId; });
        } else if (c.midi) {
            if (dst->type != TrackType::Midi) return EditResult::fail("MIDI clips can only move to MIDI tracks");
            dst->midiClips.push_back(*c.midi);
            std::erase_if(src->midiClips, [&](auto& x) { return x.id == clipId; });
        } else {
            if (dst->type != TrackType::Beat) return EditResult::fail("pattern clips can only move to beat tracks");
            dst->patternClips.push_back(*c.pattern);
            std::erase_if(src->patternClips, [&](auto& x) { return x.id == clipId; });
        }
    }
    return {true, {}, {}};
}

EditResult copyClipTo(Project& p, const std::string& clipId, const std::string& trackId, double startBeat) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    Track* dst = trackId.empty() ? c.track : p.findTrack(trackId);
    if (!dst) return EditResult::fail("target track not found");
    if (dst->locked) return EditResult::fail("target track is locked");
    const std::string nid = files::newId();
    if (c.audio) {
        if (dst->type != TrackType::Audio) return EditResult::fail("audio clips need an audio track");
        AudioClip n = *c.audio;
        n.id = nid;
        n.startBeat = std::max(0.0, startBeat);
        n.groupId.clear();
        n.locked = false;
        dst->audioClips.push_back(n);
    } else if (c.midi) {
        if (dst->type != TrackType::Midi) return EditResult::fail("MIDI clips need a MIDI track");
        MidiClip n = *c.midi;
        n.id = nid;
        n.startBeat = std::max(0.0, startBeat);
        n.groupId.clear();
        n.locked = false;
        dst->midiClips.push_back(n);
    } else {
        if (dst->type != TrackType::Beat) return EditResult::fail("pattern clips need a beat track");
        PatternClip n = *c.pattern;
        n.id = nid;
        n.startBeat = std::max(0.0, startBeat);
        n.locked = false;
        dst->patternClips.push_back(n);
    }
    return {true, {}, nid};
}

EditResult duplicateClip(Project& p, const std::string& clipId, std::optional<double> startBeat) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    const double s = startBeat.value_or(c.start() + c.length());
    return copyClipTo(p, clipId, c.track->id, s);
}

EditResult splitClip(Project& p, const std::string& clipId, double at) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    const double start = c.start(), end = c.start() + c.length();
    if (at <= start + kEps || at >= end - kEps) return EditResult::fail("split point outside clip");
    const std::string nid = files::newId();
    if (c.audio) {
        AudioClip right = *c.audio;
        right.id = nid;
        right.startBeat = at;
        right.lengthBeats = end - at;
        right.sourceOffsetSec = c.audio->sourceOffsetSec + (secs(p, at) - secs(p, start)) / c.audio->stretch;
        right.fadeInBeats = 0.0;
        c.audio->lengthBeats = at - start;
        c.audio->fadeOutBeats = 0.0;
        c.audio->fadeInBeats = std::min(c.audio->fadeInBeats, c.audio->lengthBeats);
        right.fadeOutBeats = std::min(right.fadeOutBeats, right.lengthBeats);
        c.track->audioClips.push_back(right);
    } else if (c.midi) {
        MidiClip right = *c.midi;
        right.id = nid;
        right.startBeat = at;
        right.lengthBeats = end - at;
        right.notes.clear();
        const double rel = at - start;
        MidiClip& left = *c.midi;
        // Unroll looped content so both halves keep sounding identically.
        std::vector<MidiNote> all;
        const double loop = left.loopLengthBeats > 0 ? left.loopLengthBeats : 0.0;
        if (loop > 0) {
            for (int k = 0; k * loop < left.lengthBeats && k < 10000; ++k)
                for (auto n : left.notes) {
                    if (n.startBeat >= loop) continue;
                    n.startBeat += k * loop;
                    if (n.startBeat < left.lengthBeats) all.push_back(n);
                }
        } else {
            all = left.notes;
        }
        std::vector<MidiNote> keep;
        for (auto n : all) {
            if (n.startBeat < rel) {
                n.lengthBeats = std::min(n.lengthBeats, rel - n.startBeat);
                keep.push_back(n);
            } else {
                n.startBeat -= rel;
                right.notes.push_back(n);
            }
        }
        left.notes = keep;
        left.lengthBeats = rel;
        left.loopLengthBeats = 0.0;
        right.loopLengthBeats = 0.0;
        c.track->midiClips.push_back(right);
    } else {
        PatternClip right = *c.pattern;
        right.id = nid;
        right.startBeat = at;
        right.lengthBeats = end - at;
        c.pattern->lengthBeats = at - start;
        // Keep pattern phase: the right half starts mid-pattern, which pattern
        // clips cannot express, so the split point must be on a pattern boundary.
        const Pattern* pat = p.findPattern(c.pattern->patternId);
        if (pat) {
            const double len = pat->lengthBeats();
            const double rem = std::fmod(at - start, len);
            if (rem > 1e-6 && len - rem > 1e-6) {
                c.pattern->lengthBeats = end - start; // undo
                return EditResult::fail("pattern clips can only be split on pattern boundaries");
            }
        }
        c.track->patternClips.push_back(right);
    }
    return {true, {}, nid};
}

EditResult trimClipStart(Project& p, const std::string& clipId, double newStart) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    const double end = c.start() + c.length();
    newStart = std::max(0.0, newStart);
    if (newStart >= end - kEps) return EditResult::fail("clip would be empty");
    const double delta = newStart - c.start();
    if (c.audio) {
        double off = c.audio->sourceOffsetSec + (secs(p, newStart) - secs(p, c.start())) / c.audio->stretch;
        if (off < 0) {
            // cannot extend before the start of the source
            newStart = p.tempo.secondsToBeat(secs(p, c.start()) - c.audio->sourceOffsetSec * c.audio->stretch);
            off = 0.0;
        }
        c.audio->sourceOffsetSec = off;
        c.audio->lengthBeats = end - newStart;
        c.audio->startBeat = newStart;
        c.audio->fadeInBeats = std::min(c.audio->fadeInBeats, c.audio->lengthBeats);
    } else if (c.midi) {
        // Notes keep their absolute timeline position.
        std::vector<MidiNote> keep;
        for (auto n : c.midi->notes) {
            n.startBeat -= delta;
            if (n.startBeat + n.lengthBeats <= 0) continue;
            if (n.startBeat < 0) {
                n.lengthBeats += n.startBeat;
                n.startBeat = 0;
            }
            keep.push_back(n);
        }
        c.midi->notes = keep;
        c.midi->startBeat = newStart;
        c.midi->lengthBeats = end - newStart;
    } else {
        c.pattern->startBeat = newStart;
        c.pattern->lengthBeats = end - newStart;
    }
    return {true, {}, {}};
}

EditResult trimClipEnd(Project& p, const std::string& clipId, double newEnd) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    if (newEnd <= c.start() + kEps) return EditResult::fail("clip would be empty");
    double len = newEnd - c.start();
    if (c.audio) {
        len = std::min(len, maxAudioClipLengthBeats(p, *c.audio));
        c.audio->fadeOutBeats = std::min(c.audio->fadeOutBeats, len);
    }
    c.length() = len;
    return {true, {}, {}};
}

EditResult slipClip(Project& p, const std::string& clipId, double deltaSeconds) {
    ClipRef c = findClip(p, clipId);
    if (!c || !c.audio) return EditResult::fail("audio clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    const double off = c.audio->sourceOffsetSec - deltaSeconds / c.audio->stretch;
    if (off < 0) return EditResult::fail("cannot slip before the start of the recording");
    const AudioAsset* a = p.findAsset(c.audio->assetId);
    if (a && a->frames > 0 && off >= static_cast<double>(a->frames) / a->sampleRate)
        return EditResult::fail("cannot slip past the end of the recording");
    c.audio->sourceOffsetSec = off;
    return {true, {}, {}};
}

EditResult stretchClip(Project& p, const std::string& clipId, double ratio) {
    ClipRef c = findClip(p, clipId);
    if (!c || !c.audio) return EditResult::fail("audio clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    if (ratio < 0.25 || ratio > 4.0) return EditResult::fail("stretch ratio must be within 0.25 .. 4");
    auto& a = *c.audio;
    const double startSec = secs(p, a.startBeat);
    const double durSec = secs(p, a.endBeat()) - startSec;
    const double newDur = durSec / a.stretch * ratio;
    a.lengthBeats = p.tempo.secondsToBeat(startSec + newDur) - a.startBeat;
    a.stretch = ratio;
    return {true, {}, {}};
}

EditResult deleteClip(Project& p, const std::string& clipId) {
    ClipRef c = findClip(p, clipId);
    if (!c) return EditResult::fail("clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    Track* t = c.track;
    std::erase_if(t->audioClips, [&](auto& x) { return x.id == clipId; });
    std::erase_if(t->midiClips, [&](auto& x) { return x.id == clipId; });
    std::erase_if(t->patternClips, [&](auto& x) { return x.id == clipId; });
    return {true, {}, {}};
}

#define ROY_CLIP_SETTER(fnName, argType, body)                                        \
    EditResult fnName(Project& p, const std::string& clipId, argType v) {             \
        ClipRef c = findClip(p, clipId);                                              \
        if (!c) return EditResult::fail("clip not found");                            \
        body;                                                                         \
        return {true, {}, {}};                                                        \
    }

ROY_CLIP_SETTER(setClipMuted, bool, {
    if (c.audio) c.audio->muted = v;
    else if (c.midi) c.midi->muted = v;
    else c.pattern->muted = v;
})
ROY_CLIP_SETTER(setClipLocked, bool, {
    if (c.audio) c.audio->locked = v;
    else if (c.midi) c.midi->locked = v;
    else c.pattern->locked = v;
})
ROY_CLIP_SETTER(setClipColor, uint32_t, {
    if (c.audio) c.audio->color = v;
    else if (c.midi) c.midi->color = v;
    else c.pattern->color = v;
})
ROY_CLIP_SETTER(setClipName, const std::string&, {
    if (c.audio) c.audio->name = v;
    else if (c.midi) c.midi->name = v;
    else return EditResult::fail("pattern clips are named by their pattern");
})
ROY_CLIP_SETTER(setClipGain, float, {
    if (!c.audio) return EditResult::fail("gain applies to audio clips");
    if (c.locked()) return EditResult::fail("clip is locked");
    c.audio->gainDb = std::clamp(v, -96.0f, 24.0f);
})
#undef ROY_CLIP_SETTER

EditResult setFades(Project& p, const std::string& clipId, double fadeIn, double fadeOut, std::optional<FadeCurve> inCurve,
                    std::optional<FadeCurve> outCurve) {
    ClipRef c = findClip(p, clipId);
    if (!c || !c.audio) return EditResult::fail("audio clip not found");
    if (c.locked()) return EditResult::fail("clip is locked");
    auto& a = *c.audio;
    fadeIn = std::clamp(fadeIn, 0.0, a.lengthBeats);
    fadeOut = std::clamp(fadeOut, 0.0, a.lengthBeats);
    if (fadeIn + fadeOut > a.lengthBeats) {
        const double s = a.lengthBeats / (fadeIn + fadeOut);
        fadeIn *= s;
        fadeOut *= s;
    }
    a.fadeInBeats = fadeIn;
    a.fadeOutBeats = fadeOut;
    if (inCurve) a.fadeInCurve = *inCurve;
    if (outCurve) a.fadeOutCurve = *outCurve;
    return {true, {}, {}};
}

EditResult crossfade(Project& p, const std::string& idA, const std::string& idB, double lengthBeats) {
    ClipRef a = findClip(p, idA), b = findClip(p, idB);
    if (!a || !b || !a.audio || !b.audio) return EditResult::fail("crossfade needs two audio clips");
    if (a.track != b.track) return EditResult::fail("clips must be on the same track");
    if (a.locked() || b.locked()) return EditResult::fail("clip is locked");
    AudioClip* first = a.audio->startBeat <= b.audio->startBeat ? a.audio : b.audio;
    AudioClip* second = first == a.audio ? b.audio : a.audio;
    double overlap = first->endBeat() - second->startBeat;
    if (overlap < kEps) {
        // Butt joint (or small gap): extend both clips into each other if source material allows.
        const double gap = -overlap;
        const double half = lengthBeats / 2.0;
        const double maxFirst = maxAudioClipLengthBeats(p, *first);
        const double wantFirst = first->lengthBeats + gap + half;
        const double extendSecondSec = (secs(p, second->startBeat) - secs(p, second->startBeat - half)) / second->stretch;
        if (wantFirst > maxFirst + kEps || second->sourceOffsetSec - extendSecondSec < -kEps)
            return EditResult::fail("not enough source material outside the clips for this crossfade length");
        first->lengthBeats = wantFirst;
        second->sourceOffsetSec -= extendSecondSec;
        second->lengthBeats += half;
        second->startBeat -= half;
        overlap = first->endBeat() - second->startBeat;
    }
    first->fadeOutBeats = std::min(overlap, first->lengthBeats);
    second->fadeInBeats = std::min(overlap, second->lengthBeats);
    first->fadeOutCurve = FadeCurve::EqualPower;
    second->fadeInCurve = FadeCurve::EqualPower;
    return {true, {}, {}};
}

EditResult groupClips(Project& p, const std::vector<std::string>& ids) {
    if (ids.size() < 2) return EditResult::fail("select at least two clips");
    const std::string g = files::newId();
    for (auto& id : ids) {
        ClipRef c = findClip(p, id);
        if (!c) return EditResult::fail("clip not found: " + id);
        if (c.pattern) return EditResult::fail("pattern clips cannot be grouped");
    }
    for (auto& id : ids) {
        ClipRef c = findClip(p, id);
        if (c.audio) c.audio->groupId = g;
        else if (c.midi) c.midi->groupId = g;
    }
    return {true, {}, g};
}

EditResult ungroup(Project& p, const std::string& groupId) {
    bool any = false;
    for (auto& t : p.tracks) {
        for (auto& a : t.audioClips)
            if (a.groupId == groupId) { a.groupId.clear(); any = true; }
        for (auto& m : t.midiClips)
            if (m.groupId == groupId) { m.groupId.clear(); any = true; }
    }
    return any ? EditResult{true, {}, {}} : EditResult::fail("group not found");
}

std::vector<std::string> clipsInRange(Project& p, const std::set<std::string>& trackIds, double s, double e) {
    std::vector<std::string> out;
    for (auto& t : p.tracks) {
        if (!trackIds.empty() && !trackIds.count(t.id)) continue;
        for (auto& c : t.audioClips) if (c.startBeat < e && c.endBeat() > s) out.push_back(c.id);
        for (auto& c : t.midiClips) if (c.startBeat < e && c.endBeat() > s) out.push_back(c.id);
        for (auto& c : t.patternClips) if (c.startBeat < e && c.endBeat() > s) out.push_back(c.id);
    }
    return out;
}

std::string addMarker(Project& p, double beat, const std::string& name) {
    Marker m;
    m.id = files::newId();
    m.beat = std::max(0.0, beat);
    m.name = name;
    p.markers.push_back(m);
    std::sort(p.markers.begin(), p.markers.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    return m.id;
}

bool removeMarker(Project& p, const std::string& id) { return std::erase_if(p.markers, [&](auto& m) { return m.id == id; }) > 0; }

std::string addSection(Project& p, const std::string& name, const std::string& type, double s, double e) {
    Section x;
    x.id = files::newId();
    x.name = name;
    x.type = type;
    x.startBeat = std::min(s, e);
    x.endBeat = std::max(s, e);
    p.sections.push_back(x);
    std::sort(p.sections.begin(), p.sections.end(), [](auto& a, auto& b) { return a.startBeat < b.startBeat; });
    return x.id;
}

bool removeSection(Project& p, const std::string& id) { return std::erase_if(p.sections, [&](auto& s) { return s.id == id; }) > 0; }

void setLoop(Project& p, bool enabled, double s, double e) {
    p.loop.enabled = enabled;
    p.loop.startBeat = std::max(0.0, std::min(s, e));
    p.loop.endBeat = std::max(s, e);
}

} // namespace roy::arrange
