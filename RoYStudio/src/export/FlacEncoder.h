#pragma once
// Native FLAC encoder (lossless): fixed-predictor subframes (orders 0-4) or
// verbatim, Rice-coded residuals with partition search, left/side/mid-side
// stereo decorrelation. Output is standard FLAC readable by any decoder.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace roy::flac {

struct EncodeOptions {
    int blockSize = 4096;
    int maxRicePartitionOrder = 6;
    bool stereoDecorrelation = true;
};

// `samples[c][i]` are integer PCM values in the range of `bitsPerSample` (16 or 24).
bool encode(const std::filesystem::path& path, const std::vector<std::vector<int32_t>>& samples, int sampleRate, int bitsPerSample,
            const EncodeOptions& opt = {}, std::string* error = nullptr);
std::vector<uint8_t> encodeToMemory(const std::vector<std::vector<int32_t>>& samples, int sampleRate, int bitsPerSample,
                                    const EncodeOptions& opt = {});

} // namespace roy::flac
