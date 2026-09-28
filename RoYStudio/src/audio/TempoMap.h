#pragma once
// Musical time <-> seconds <-> samples.
// "Beat" always means a quarter note. Tempo is piecewise constant; tempo automation
// (ramps / curves) is rendered into fine constant steps (see automation::renderTempo).
// Conversions are O(log n) via a cumulative-seconds table rebuilt on every edit.
#include <cstdint>
#include <vector>

namespace roy {

struct TempoEvent {
    double beat = 0.0; // position in quarter notes where this tempo starts
    double bpm = 120.0;
};

struct TimeSigEvent {
    int bar = 0; // zero-based bar index where the signature starts
    int numerator = 4;
    int denominator = 4;
};

struct BarBeat {
    int bar = 0;        // zero-based
    double beatInBar = 0; // in units of the signature denominator, zero-based
};

class TempoMap {
public:
    TempoMap() = default;
    explicit TempoMap(double bpm, int num = 4, int den = 4);

    void setTempo(double bpm); // replaces all tempo events with a single tempo
    void addTempoEvent(double beat, double bpm);
    // Replaces all tempo events (sorted, clamped, first one moved to beat 0).
    void setTempoEvents(std::vector<TempoEvent> events);
    void setTimeSignature(int num, int den); // replaces all signature events
    void addTimeSignature(int bar, int num, int den);

    const std::vector<TempoEvent>& tempoEvents() const { return tempo_; }
    const std::vector<TimeSigEvent>& timeSignatures() const { return sigs_; }
    double tempoAt(double beat) const;
    TimeSigEvent signatureAtBar(int bar) const;

    double beatToSeconds(double beat) const;
    double secondsToBeat(double seconds) const;
    double beatToSample(double beat, double sampleRate) const { return beatToSeconds(beat) * sampleRate; }
    double sampleToBeat(double sample, double sampleRate) const { return secondsToBeat(sample / sampleRate); }

    // Length of a bar in quarter notes, e.g. 4/4 -> 4, 6/8 -> 3.
    static double barLengthBeats(int num, int den) { return num * 4.0 / den; }
    double barToBeat(int bar) const;
    BarBeat beatToBarBeat(double beat) const;

private:
    void rebuildCache();
    size_t eventAtBeat(double beat) const;
    std::vector<TempoEvent> tempo_{TempoEvent{0.0, 120.0}};
    std::vector<double> startSeconds_{0.0}; // seconds at tempo_[i].beat
    std::vector<TimeSigEvent> sigs_{TimeSigEvent{0, 4, 4}};
};

} // namespace roy
