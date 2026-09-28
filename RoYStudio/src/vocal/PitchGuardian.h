#pragma once
// PITCH GUARDIAN - pitch correction that respects natural movement.
//
// Modes:  OFF    no correction
//         WARN   analysis + warnings only
//         ASSIST corrects notes deviating more than a threshold
//         LOCK   binds every note to the scale (or chromatic grid)
// OFF-KEY FILTER: automatic correction never pulls to a note outside the key.
// ALLOW CHROMATIC NOTES: notes clearly sung on an out-of-key semitone stay there.
// Low-confidence frames are corrected less (or not at all); vibrato and slides
// are preserved according to their settings. The original audio is never
// modified: correction renders a new buffer.
#include "midi/Scale.h"
#include "vocal/PitchAnalysis.h"

#include <string>
#include <vector>

namespace roy::vocal {

enum class GuardianMode { Off, Warn, Assist, Lock };
const char* guardianModeId(GuardianMode m);
GuardianMode guardianModeFromId(const std::string& s);

struct PitchGuardianSettings {
    GuardianMode mode = GuardianMode::Assist;
    Key key;
    double strength = 1.0;            // 0..1 correction amount
    double speedMs = 30.0;            // retune time constant (0 = instant)
    double humanize = 0.5;            // 0 = flatten micro pitch variation, 1 = keep it (only the centre moves)
    bool formantPreserve = true;
    double vibratoPreserve = 1.0;     // 1 = keep vibrato depth
    double slidePreserve = 1.0;       // 1 = keep slide shapes
    double transitionMs = 60.0;
    bool scaleLock = false;           // force scale targets (implied by LOCK)
    bool offKeyFilter = true;         // OFF-KEY NOTES button
    bool allowChromatic = false;      // ALLOW CHROMATIC NOTES
    double assistThresholdCents = 25.0;
    double chromaticToleranceCents = 30.0;
    double minConfidence = 0.75;      // below: correction fades out
};

struct NoteCorrection {
    size_t note = 0;
    double fromMidi = 0;
    double toMidi = 0;
    double shift = 0;      // semitones applied to the note centre
    bool corrected = false;
    std::string reason;    // why (not) corrected
};

struct CorrectionPlan {
    double hopSeconds = 0;
    std::vector<double> shift;          // semitones per pitch frame
    std::vector<NoteCorrection> notes;
    std::vector<TuningIssue> warnings;
    bool any() const;
};

CorrectionPlan planCorrection(const PitchTrack& track, const PitchAnalysis& analysis, const PitchGuardianSettings& s);

// Renders corrected audio (TD-PSOLA). Samples where no correction is planned
// are passed through bit-identically.
std::vector<std::vector<float>> applyCorrection(const std::vector<std::vector<float>>& audio, double sampleRate,
                                                const PitchTrack& track, const CorrectionPlan& plan, bool formantPreserve);

// Convenience: detect -> analyse -> plan -> render.
struct GuardianResult {
    PitchTrack track;
    PitchAnalysis analysis;
    CorrectionPlan plan;
    std::vector<std::vector<float>> audio;
};
GuardianResult runPitchGuardian(const std::vector<std::vector<float>>& audio, double sampleRate, const PitchGuardianSettings& s);

// Pitch shift with an arbitrary per-frame curve (used by Double Magnet pitch alignment, RoY Pitch).
std::vector<std::vector<float>> psolaShift(const std::vector<std::vector<float>>& audio, double sampleRate,
                                           const PitchTrack& track, const std::vector<double>& shiftPerFrame,
                                           bool formantPreserve);

} // namespace roy::vocal
