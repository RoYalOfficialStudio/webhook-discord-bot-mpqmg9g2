#pragma once
// Offline time-stretching (WSOLA) and pitch shifting (WSOLA + resampling).
// All channels share the same alignment decisions, so stereo images stay intact.
// Limitation: the resampling pitch shifter moves formants with the pitch; the
// formant-preserving path for vocals is the PSOLA shifter in vocal/.
#include <cstdint>
#include <functional>
#include <vector>

namespace roy::dsp {

using Channels = std::vector<std::vector<float>>;

// ratio > 1 => longer (slower), pitch unchanged.
Channels timeStretch(const Channels& in, double ratio, double sampleRate);
// Shifts pitch by `semitones`, duration unchanged.
Channels pitchShift(const Channels& in, double semitones, double sampleRate);
// Variable time warp: output sample o reads input around inputPosForOutput(o) (WSOLA aligned).
Channels timeWarp(const Channels& in, int64_t outLen, const std::function<double(int64_t)>& inputPosForOutput, double sampleRate);
// Both at once (single pass of WSOLA + one resample).
Channels stretchAndShift(const Channels& in, double ratio, double semitones, double sampleRate);

} // namespace roy::dsp
