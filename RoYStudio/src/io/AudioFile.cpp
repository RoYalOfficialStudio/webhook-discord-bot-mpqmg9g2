#include "io/AudioFile.h"
#include <fstream>
#include "dsp/Resampler.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace roy {

namespace fs = std::filesystem;

const char* sampleFormatId(SampleFormat f) {
    switch (f) {
    case SampleFormat::Pcm16: return "pcm16";
    case SampleFormat::Pcm24: return "pcm24";
    case SampleFormat::Pcm32: return "pcm32";
    case SampleFormat::Float32: return "float32";
    }
    return "pcm24";
}

int bitsPerSample(SampleFormat f) {
    switch (f) {
    case SampleFormat::Pcm16: return 16;
    case SampleFormat::Pcm24: return 24;
    case SampleFormat::Pcm32: return 32;
    case SampleFormat::Float32: return 32;
    }
    return 24;
}

namespace {
ma_result initDecoder(const fs::path& path, ma_decoder* dec) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
#ifdef _WIN32
    return ma_decoder_init_file_w(path.wstring().c_str(), &cfg, dec);
#else
    return ma_decoder_init_file(path.string().c_str(), &cfg, dec);
#endif
}
} // namespace

bool probeAudioFile(const fs::path& path, AudioFileInfo& info, std::string* error) {
    ma_decoder dec;
    if (initDecoder(path, &dec) != MA_SUCCESS) {
        if (error) *error = "cannot decode " + path.string();
        return false;
    }
    ma_uint64 frames = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &frames);
    info.sampleRate = dec.outputSampleRate;
    info.channels = static_cast<int>(dec.outputChannels);
    info.frames = static_cast<int64_t>(frames);
    ma_decoder_uninit(&dec);
    return true;
}

namespace {
// Gapless MP3: the LAME/Xing info frame stores encoder delay and padding. The decoder
// outputs the (silent) info frame plus encoder delay + 529 samples decoder delay at the
// start, and padding - 529 extra samples at the end. Returns false if no LAME tag.
bool mp3GaplessTrim(const fs::path& path, int64_t decodedFrames, int64_t& trimStart, int64_t& trimEnd) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> h(64 * 1024);
    f.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()));
    const size_t n = static_cast<size_t>(f.gcount());
    size_t pos = 0;
    if (n >= 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') // skip ID3v2 (synch-safe size)
        pos = 10 + ((h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f));
    while (pos + 4 < n && !(h[pos] == 0xFF && (h[pos + 1] & 0xE0) == 0xE0)) ++pos; // first frame sync
    if (pos + 200 >= n) return false;
    const bool mpeg1 = (h[pos + 1] & 0x18) == 0x18;
    const bool mono = (h[pos + 3] & 0xC0) == 0xC0;
    const size_t sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const size_t x = pos + 4 + sideInfo;
    if (std::memcmp(&h[x], "Xing", 4) != 0 && std::memcmp(&h[x], "Info", 4) != 0) return false;
    // LAME extension follows the Xing fields; find it within the frame
    size_t lame = 0;
    for (size_t i = x; i + 4 < std::min(n, x + 200); ++i)
        if (std::memcmp(&h[i], "LAME", 4) == 0 || std::memcmp(&h[i], "Lavc", 4) == 0) {
            lame = i;
            break;
        }
    if (!lame || lame + 24 > n) return false;
    const int encDelay = (h[lame + 21] << 4) | (h[lame + 22] >> 4);
    const int padding = ((h[lame + 22] & 0x0F) << 8) | h[lame + 23];
    const int frameSamples = mpeg1 ? 1152 : 576;
    // Did the decoder output the (silent) info frame? The Xing frame count excludes it.
    int64_t infoFrame = frameSamples;
    const uint32_t flags = (uint32_t(h[x + 4]) << 24) | (uint32_t(h[x + 5]) << 16) | (uint32_t(h[x + 6]) << 8) | h[x + 7];
    if (flags & 1u) {
        const int64_t frames = (int64_t(h[x + 8]) << 24) | (int64_t(h[x + 9]) << 16) | (int64_t(h[x + 10]) << 8) | h[x + 11];
        if (decodedFrames == frames * frameSamples) infoFrame = 0;
        else if (decodedFrames != (frames + 1) * frameSamples) return false; // unexpected decoder behaviour: do not trim
    }
    trimStart = infoFrame + encDelay + 529;
    trimEnd = std::max(0, padding - 529);
    return true;
}
} // namespace

bool readAudioFile(const fs::path& path, AudioData& out, std::string* error) {
    ma_decoder dec;
    if (initDecoder(path, &dec) != MA_SUCCESS) {
        if (error) *error = "cannot decode " + path.string();
        return false;
    }
    const int ch = static_cast<int>(dec.outputChannels);
    out.sampleRate = dec.outputSampleRate;
    out.numChannels = ch;
    out.channels.assign(static_cast<size_t>(ch), {});
    std::vector<float> buf(static_cast<size_t>(4096 * ch));
    int64_t total = 0;
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, buf.data(), 4096, &got);
        for (ma_uint64 i = 0; i < got; ++i)
            for (int c = 0; c < ch; ++c) out.channels[static_cast<size_t>(c)].push_back(buf[static_cast<size_t>(i) * ch + c]);
        total += static_cast<int64_t>(got);
        if (got < 4096 || r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&dec);
    out.numFrames = total;
    std::string ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int64_t t0 = 0, t1 = 0;
    if (ext == ".mp3" && mp3GaplessTrim(path, total, t0, t1) && t0 + t1 < total) {
        for (auto& c : out.channels) {
            c.erase(c.end() - t1, c.end());
            c.erase(c.begin(), c.begin() + t0);
        }
        out.numFrames = total - t0 - t1;
    }
    return true;
}

std::shared_ptr<AudioData> loadAudioAt(const fs::path& path, double targetRate, std::string* error) {
    auto d = std::make_shared<AudioData>();
    if (!readAudioFile(path, *d, error)) return nullptr;
    if (targetRate > 0 && std::fabs(d->sampleRate - targetRate) > 0.5) {
        for (auto& c : d->channels) c = dsp::resample(c, d->sampleRate, targetRate);
        d->sampleRate = targetRate;
        d->numFrames = d->channels.empty() ? 0 : static_cast<int64_t>(d->channels[0].size());
    }
    return d;
}

// ---------------------------------------------------------------- WavWriter
WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const fs::path& path, double sampleRate, int channels, SampleFormat fmt, bool allowOverwrite,
                     std::string* error) {
    close();
    std::error_code ec;
    if (!allowOverwrite && fs::exists(path, ec)) {
        if (error) *error = "refusing to overwrite existing file " + path.string();
        return false;
    }
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    file_ = _wfopen(path.wstring().c_str(), L"wb");
#else
    file_ = std::fopen(path.string().c_str(), "wb");
#endif
    if (!file_) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    path_ = path;
    sampleRate_ = sampleRate;
    channels_ = std::max(1, channels);
    fmt_ = fmt;
    frames_ = 0;
    framesAtLastHeader_ = 0;
    return writeHeader();
}

bool WavWriter::writeHeader() {
    if (!file_) return false;
    const int bps = bitsPerSample(fmt_);
    const uint32_t blockAlign = static_cast<uint32_t>(channels_ * bps / 8);
    const uint64_t dataBytes64 = static_cast<uint64_t>(frames_) * blockAlign;
    const uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(dataBytes64, 0xFFFFFFFFull - 36));
    uint8_t h[44];
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) h[off + i] = static_cast<uint8_t>(v >> (8 * i)); };
    auto put16 = [&](int off, uint16_t v) { h[off] = static_cast<uint8_t>(v); h[off + 1] = static_cast<uint8_t>(v >> 8); };
    std::memcpy(h, "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, fmt_ == SampleFormat::Float32 ? 3 : 1);
    put16(22, static_cast<uint16_t>(channels_));
    put32(24, static_cast<uint32_t>(std::lround(sampleRate_)));
    put32(28, static_cast<uint32_t>(std::lround(sampleRate_)) * blockAlign);
    put16(32, static_cast<uint16_t>(blockAlign));
    put16(34, static_cast<uint16_t>(bps));
    std::memcpy(h + 36, "data", 4);
    put32(40, dataBytes);
    const long pos = std::ftell(file_);
    std::fseek(file_, 0, SEEK_SET);
    const bool ok = std::fwrite(h, 1, 44, file_) == 44;
    if (pos > 44) std::fseek(file_, pos, SEEK_SET);
    else std::fseek(file_, 0, SEEK_END);
    return ok;
}

void WavWriter::encode(float v, uint8_t* dst) const {
    if (!std::isfinite(v)) v = 0.0f;
    switch (fmt_) {
    case SampleFormat::Float32: std::memcpy(dst, &v, 4); break;
    case SampleFormat::Pcm16: {
        const int32_t s = static_cast<int32_t>(std::lrint(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        dst[0] = static_cast<uint8_t>(s);
        dst[1] = static_cast<uint8_t>(s >> 8);
        break;
    }
    case SampleFormat::Pcm24: {
        const int32_t s = static_cast<int32_t>(std::lrint(std::clamp(static_cast<double>(v), -1.0, 1.0) * 8388607.0));
        dst[0] = static_cast<uint8_t>(s);
        dst[1] = static_cast<uint8_t>(s >> 8);
        dst[2] = static_cast<uint8_t>(s >> 16);
        break;
    }
    case SampleFormat::Pcm32: {
        const int32_t s = static_cast<int32_t>(std::llrint(std::clamp(static_cast<double>(v), -1.0, 1.0) * 2147483647.0));
        for (int i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(s >> (8 * i));
        break;
    }
    }
}

bool WavWriter::writePlanar(const float* const* channels, int numFrames) {
    if (!file_ || numFrames <= 0) return file_ != nullptr;
    const int bytes = bitsPerSample(fmt_) / 8;
    scratch_.resize(static_cast<size_t>(numFrames * channels_ * bytes));
    uint8_t* p = scratch_.data();
    for (int i = 0; i < numFrames; ++i)
        for (int c = 0; c < channels_; ++c) {
            encode(channels[c][i], p);
            p += bytes;
        }
    const bool ok = std::fwrite(scratch_.data(), 1, scratch_.size(), file_) == scratch_.size();
    frames_ += numFrames;
    if (frames_ - framesAtLastHeader_ > static_cast<int64_t>(sampleRate_)) {
        std::fflush(file_);
        writeHeader();
        framesAtLastHeader_ = frames_;
    }
    return ok;
}

bool WavWriter::writeInterleaved(const float* data, int numFrames) {
    if (!file_ || numFrames <= 0) return file_ != nullptr;
    const int bytes = bitsPerSample(fmt_) / 8;
    scratch_.resize(static_cast<size_t>(numFrames * channels_ * bytes));
    uint8_t* p = scratch_.data();
    for (int i = 0; i < numFrames * channels_; ++i) {
        encode(data[i], p);
        p += bytes;
    }
    const bool ok = std::fwrite(scratch_.data(), 1, scratch_.size(), file_) == scratch_.size();
    frames_ += numFrames;
    return ok;
}

bool WavWriter::close() {
    if (!file_) return true;
    bool ok = writeHeader();
    ok = std::fflush(file_) == 0 && ok;
    std::fclose(file_);
    file_ = nullptr;
    return ok;
}

bool writeWavFile(const fs::path& path, const std::vector<std::vector<float>>& channels, double sampleRate,
                  SampleFormat fmt, bool allowOverwrite, std::string* error) {
    if (channels.empty()) {
        if (error) *error = "no channels";
        return false;
    }
    WavWriter w;
    if (!w.open(path, sampleRate, static_cast<int>(channels.size()), fmt, allowOverwrite, error)) return false;
    const size_t frames = channels[0].size();
    std::vector<const float*> ptrs;
    for (auto& c : channels) ptrs.push_back(c.data());
    const size_t chunk = 65536;
    for (size_t off = 0; off < frames; off += chunk) {
        const int n = static_cast<int>(std::min(chunk, frames - off));
        std::vector<const float*> p2;
        for (auto* p : ptrs) p2.push_back(p + off);
        if (!w.writePlanar(p2.data(), n)) {
            if (error) *error = "write error";
            return false;
        }
    }
    return w.close();
}

} // namespace roy
