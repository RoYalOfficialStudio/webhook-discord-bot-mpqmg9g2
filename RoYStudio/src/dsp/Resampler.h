#pragma once
// Offline band-limited resampler (Kaiser-windowed sinc). Used on the message
// thread when importing assets or exporting at another sample rate.
#include <vector>

namespace roy::dsp {

// Resamples one channel. quality: number of zero crossings per side (8..64).
std::vector<float> resample(const std::vector<float>& in, double srIn, double srOut, int quality = 32);

} // namespace roy::dsp
