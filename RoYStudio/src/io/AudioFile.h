#pragma once
// Audio file reading (WAV/FLAC/MP3 via miniaudio's decoders) and WAV writing.
#include "audio/RenderGraph.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace roy {

enum class SampleFormat { Pcm16, Pcm24, Pcm32, Float32 };
const char* sampleFormatId(SampleFormat f);
int bitsPerSample(SampleFormat f);

// Decodes a whole file into planar float at its native rate.
bool readAudioFile(const std::filesystem::path& path, AudioData& out, std::string* error = nullptr);
// Decodes and, if needed, resamples to `targetRate`.
std::shared_ptr<AudioData> loadAudioAt(const std::filesystem::path& path, double targetRate, std::string* error = nullptr);

struct AudioFileInfo {
    double sampleRate = 0;
    int channels = 0;
    int64_t frames = 0;
};
bool probeAudioFile(const std::filesystem::path& path, AudioFileInfo& info, std::string* error = nullptr);

// Streaming WAV writer. Refuses to overwrite existing files unless allowed.
// Writes a RIFF header (RF64-free, max 4 GB) and patches sizes on close();
// the header is also refreshed periodically so a crash leaves a readable file.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::filesystem::path& path, double sampleRate, int channels, SampleFormat fmt,
              bool allowOverwrite = false, std::string* error = nullptr);
    // Planar input: channels[c][i].
    bool writePlanar(const float* const* channels, int numFrames);
    bool writeInterleaved(const float* data, int numFrames);
    bool close();
    bool isOpen() const { return file_ != nullptr; }
    // True after a short write (disk full / removed drive). The file on disk stays a
    // valid WAV holding every frame that was written before the failure.
    bool failed() const { return failed_; }
    int64_t framesWritten() const { return frames_; }
    const std::filesystem::path& path() const { return path_; }

private:
    bool writeHeader();
    bool commit(int numFrames);
    void encode(float v, uint8_t* dst) const;

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    double sampleRate_ = 48000;
    int channels_ = 2;
    SampleFormat fmt_ = SampleFormat::Pcm24;
    int64_t frames_ = 0;
    int64_t framesAtLastHeader_ = 0;
    bool failed_ = false;
    std::vector<uint8_t> scratch_;
};

bool writeWavFile(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels, double sampleRate,
                  SampleFormat fmt, bool allowOverwrite = false, std::string* error = nullptr);

} // namespace roy
