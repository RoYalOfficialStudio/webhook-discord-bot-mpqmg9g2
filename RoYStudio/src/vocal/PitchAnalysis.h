#pragma once
// Note segmentation and classification of a pitch track:
// stable note / vibrato / slide / transition / unvoiced / uncertain.
#include "midi/Scale.h"
#include "vocal/PitchDetector.h"

#include <string>
#include <vector>

namespace roy::vocal {

enum class FrameKind { Unvoiced, Uncertain, Stable, Vibrato, Slide, Transition };
const char* frameKindName(FrameKind k);

struct NoteSegment {
    size_t firstFrame = 0, lastFrame = 0; // inclusive
    double start = 0, end = 0;            // seconds
    double medianMidi = 0;
    double meanConfidence = 0;
    FrameKind kind = FrameKind::Stable;   // Stable, Vibrato, Slide or Transition
    double vibratoRateHz = 0;
    double vibratoExtentCents = 0;        // peak deviation from the centre
    double slideSemitones = 0;            // total glide (signed)
    int nearestNote = 0;                  // nearest semitone
    double centsFromNearest = 0;          // + = sharp
    double duration() const { return end - start; }
};

struct PitchAnalysisSettings {
    double uncertainConfidence = 0.7; // below: frame is "Uncertain" -> not corrected aggressively
    double splitSemitones = 1.0;
    double splitMinSeconds = 0.045;
    double transitionSeconds = 0.06;
};

struct PitchAnalysis {
    std::vector<FrameKind> kinds;     // per frame
    std::vector<int> noteOfFrame;     // index into notes, -1 = none
    std::vector<NoteSegment> notes;
    double voicedSeconds = 0;
    double sustainedRatio = 0;        // voiced time inside stable/vibrato notes >= 150 ms
    double meanNoteSeconds = 0;
    double rapIndicator = 0;          // 1 - sustainedRatio: technical indicator only, not a judgement
    double inTuneRatio = 0;           // stable notes within +-25 cents of the nearest semitone
};

PitchAnalysis analyzePitch(const PitchTrack& t, const PitchAnalysisSettings& s = {});

// Per-note tuning report against a key (for WARN mode / Vocal Doctor).
struct TuningIssue {
    size_t note = 0;
    double time = 0;
    double centsOff = 0;   // deviation from the target note
    bool offKey = false;   // nearest semitone is outside the key
    std::string text;
};
std::vector<TuningIssue> tuningIssues(const PitchAnalysis& a, const Key& key, double thresholdCents = 30.0);

} // namespace roy::vocal
