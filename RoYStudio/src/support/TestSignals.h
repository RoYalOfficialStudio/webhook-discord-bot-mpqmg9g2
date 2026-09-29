#pragma once
// SELF-GENERATED TEST MATERIAL (no third-party samples, no copyright issues): test tone,
// a synthetic "vocal" melody with deliberately off-key / out-of-tune notes for the pitch tools,
// and simple drum one-shots (kick, snare, clap, hi-hats, 808). Everything is synthesised here.
#include <filesystem>
#include <string>
#include <vector>

namespace roy::support {

std::vector<float> testTone(double sampleRate, double freq, double seconds, float amplitude = 0.25f);
// A-minor phrase (formant-shaped harmonics, vibrato). Note 3 is 35 cents sharp, note 6 is an
// out-of-key F#, note 8 is 25 cents flat - so OFF KEY / correction are visible.
std::vector<float> syntheticVocal(double sampleRate, double seconds = 8.0);
std::vector<float> drumHit(const std::string& kind, double sampleRate); // kick | snare | clap | hat | openhat | 808

// Writes the whole test kit content below `folder` (Audio/, Samples/Drums/, ...). Returns the
// written files (relative paths). Existing files are replaced only if they were written by us.
std::vector<std::string> writeTestSignals(const std::filesystem::path& folder, std::string* error = nullptr);

} // namespace roy::support
