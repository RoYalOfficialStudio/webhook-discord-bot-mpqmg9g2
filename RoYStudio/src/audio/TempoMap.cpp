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

double TempoMap::tempoAt(double beat) const {
    double bpm = tempo_.front().bpm;
    for (auto& e : tempo_) {
        if (e.beat <= beat) bpm = e.bpm;
        else break;
    }
    return bpm;
}

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
    double seconds = 0.0;
    for (size_t i = 0; i < tempo_.size(); ++i) {
        const double start = tempo_[i].beat;
        const double end = i + 1 < tempo_.size() ? tempo_[i + 1].beat : beat;
        if (beat <= start) break;
        const double segEnd = std::min(beat, end);
        seconds += (segEnd - start) * 60.0 / tempo_[i].bpm;
        if (beat <= end) break;
    }
    return seconds;
}

double TempoMap::secondsToBeat(double seconds) const {
    if (seconds <= 0.0) return seconds * tempo_.front().bpm / 60.0;
    double elapsed = 0.0;
    for (size_t i = 0; i < tempo_.size(); ++i) {
        const double spb = 60.0 / tempo_[i].bpm;
        if (i + 1 < tempo_.size()) {
            const double segSeconds = (tempo_[i + 1].beat - tempo_[i].beat) * spb;
            if (seconds < elapsed + segSeconds) return tempo_[i].beat + (seconds - elapsed) / spb;
            elapsed += segSeconds;
        } else {
            return tempo_[i].beat + (seconds - elapsed) / spb;
        }
    }
    return 0.0;
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
