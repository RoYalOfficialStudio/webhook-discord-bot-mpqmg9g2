#include "audio/TempoMap.h"

#include <algorithm>
#include <cmath>

namespace roy {

TempoMap::TempoMap(double bpm, int num, int den) {
    setTempo(bpm);
    setTimeSignature(num, den);
}

void TempoMap::setTempo(double bpm) {
    tempo_.assign(1, TempoEvent{0.0, std::clamp(bpm, 10.0, 999.0)});
    rebuildCache();
}

void TempoMap::setTempoEvents(std::vector<TempoEvent> events) {
    if (events.empty()) events.push_back({0.0, 120.0});
    for (auto& e : events) {
        e.bpm = std::isfinite(e.bpm) ? std::clamp(e.bpm, 10.0, 999.0) : 120.0;
        e.beat = std::isfinite(e.beat) ? std::max(0.0, e.beat) : 0.0;
    }
    std::stable_sort(events.begin(), events.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    tempo_.clear();
    for (auto& e : events) {
        if (!tempo_.empty() && std::fabs(tempo_.back().beat - e.beat) < 1e-9) tempo_.back().bpm = e.bpm;
        else tempo_.push_back(e);
    }
    if (tempo_.front().beat > 0.0) tempo_.insert(tempo_.begin(), TempoEvent{0.0, tempo_.front().bpm});
    rebuildCache();
}

void TempoMap::rebuildCache() {
    startSeconds_.resize(tempo_.size());
    double seconds = 0.0;
    for (size_t i = 0; i < tempo_.size(); ++i) {
        startSeconds_[i] = seconds;
        if (i + 1 < tempo_.size()) seconds += (tempo_[i + 1].beat - tempo_[i].beat) * 60.0 / tempo_[i].bpm;
    }
}

// Index of the last event starting at or before `beat` (0 for beats before the first).
size_t TempoMap::eventAtBeat(double beat) const {
    auto it = std::upper_bound(tempo_.begin(), tempo_.end(), beat, [](double b, const TempoEvent& e) { return b < e.beat; });
    return it == tempo_.begin() ? 0 : static_cast<size_t>(it - tempo_.begin()) - 1;
}

void TempoMap::addTempoEvent(double beat, double bpm) {
    bpm = std::clamp(bpm, 10.0, 999.0);
    beat = std::max(0.0, beat);
    auto it = std::find_if(tempo_.begin(), tempo_.end(), [&](const TempoEvent& e) { return std::fabs(e.beat - beat) < 1e-9; });
    if (it != tempo_.end()) {
        it->bpm = bpm;
    } else {
        tempo_.push_back({beat, bpm});
        std::sort(tempo_.begin(), tempo_.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    }
    if (tempo_.front().beat > 0.0) tempo_.insert(tempo_.begin(), TempoEvent{0.0, tempo_.front().bpm});
    rebuildCache();
}

void TempoMap::setTimeSignature(int num, int den) {
    sigs_.assign(1, TimeSigEvent{0, std::clamp(num, 1, 64), std::clamp(den, 1, 64)});
}

void TempoMap::addTimeSignature(int bar, int num, int den) {
    bar = std::max(0, bar);
    auto it = std::find_if(sigs_.begin(), sigs_.end(), [&](auto& s) { return s.bar == bar; });
    if (it != sigs_.end()) {
        it->numerator = num;
        it->denominator = den;
    } else {
        sigs_.push_back({bar, num, den});
        std::sort(sigs_.begin(), sigs_.end(), [](auto& a, auto& b) { return a.bar < b.bar; });
    }
}

double TempoMap::tempoAt(double beat) const { return tempo_[eventAtBeat(beat)].bpm; }

TimeSigEvent TempoMap::signatureAtBar(int bar) const {
    TimeSigEvent s = sigs_.front();
    for (auto& e : sigs_) {
        if (e.bar <= bar) s = e;
        else break;
    }
    return s;
}

double TempoMap::beatToSeconds(double beat) const {
    // Negative positions (count-in/pre-roll before zero) use the first tempo.
    if (beat <= 0.0) return beat * 60.0 / tempo_.front().bpm;
    const size_t i = eventAtBeat(beat);
    return startSeconds_[i] + (beat - tempo_[i].beat) * 60.0 / tempo_[i].bpm;
}

double TempoMap::secondsToBeat(double seconds) const {
    if (seconds <= 0.0) return seconds * tempo_.front().bpm / 60.0;
    auto it = std::upper_bound(startSeconds_.begin(), startSeconds_.end(), seconds);
    const size_t i = it == startSeconds_.begin() ? 0 : static_cast<size_t>(it - startSeconds_.begin()) - 1;
    return tempo_[i].beat + (seconds - startSeconds_[i]) * tempo_[i].bpm / 60.0;
}

double TempoMap::barToBeat(int bar) const {
    if (bar <= 0) {
        const auto s = sigs_.front();
        return bar * barLengthBeats(s.numerator, s.denominator);
    }
    double beat = 0.0;
    for (size_t i = 0; i < sigs_.size(); ++i) {
        const int start = sigs_[i].bar;
        const int end = i + 1 < sigs_.size() ? sigs_[i + 1].bar : bar;
        const int segEnd = std::min(bar, end);
        if (segEnd > start) beat += (segEnd - start) * barLengthBeats(sigs_[i].numerator, sigs_[i].denominator);
        if (bar <= end) break;
    }
    return beat;
}

BarBeat TempoMap::beatToBarBeat(double beat) const {
    BarBeat out;
    if (beat < 0.0) {
        const auto s = sigs_.front();
        const double len = barLengthBeats(s.numerator, s.denominator);
        const int bar = static_cast<int>(std::floor(beat / len));
        out.bar = bar;
        out.beatInBar = (beat - bar * len) * s.denominator / 4.0;
        return out;
    }
    double pos = 0.0;
    for (size_t i = 0; i < sigs_.size(); ++i) {
        const double len = barLengthBeats(sigs_[i].numerator, sigs_[i].denominator);
        const bool last = i + 1 == sigs_.size();
        const double segBeats = last ? 1e300 : (sigs_[i + 1].bar - sigs_[i].bar) * len;
        if (beat < pos + segBeats) {
            const double rel = beat - pos;
            const int barsIn = static_cast<int>(std::floor(rel / len + 1e-9));
            out.bar = sigs_[i].bar + barsIn;
            out.beatInBar = std::max(0.0, (rel - barsIn * len)) * sigs_[i].denominator / 4.0;
            return out;
        }
        pos += segBeats;
    }
    return out;
}

} // namespace roy
