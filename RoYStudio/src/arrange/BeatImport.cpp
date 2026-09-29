#include "arrange/BeatImport.h"
#include "audio/RenderGraph.h"
#include "io/AudioFile.h"
#include "sampler/SampleTools.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace roy::beatimport {

std::optional<double> bpmFromFileName(const std::string& stem) {
    static const std::regex re(R"((\d{2,3}(?:[.,]\d+)?)\s*[-_ ]?\s*bpm|bpm\s*[-_ :]?\s*(\d{2,3}(?:[.,]\d+)?))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(stem, m, re)) return std::nullopt;
    std::string num = m[1].matched ? m[1].str() : m[2].str();
    std::replace(num.begin(), num.end(), ',', '.');
    const double bpm = std::stod(num);
    if (bpm < 40 || bpm > 300) return std::nullopt;
    return bpm;
}

std::optional<Key> keyFromFileName(const std::string& stem) {
    // note letter (upper case), optional #/b, then a mode word; nothing letter-like after it
    static const std::regex re(R"((?:^|[^A-Za-z])([A-G])([#b]?)[ _-]?(minor|min|m|major|maj)(?![A-Za-z]))", std::regex::icase);
    for (auto it = std::sregex_iterator(stem.begin(), stem.end(), re); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const std::string letter = m[1].str();
        if (!std::isupper(static_cast<unsigned char>(letter[0]))) continue; // "a min" is not a key, "A min" is
        std::string mode = m[3].str();
        for (auto& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // "m" must be lower case ("AM" is ambiguous with am/pm, "M" often means major) -> only "m"
        if (mode == "m" && m[3].str() != "m") continue;
        const auto pc = pitchClassFromName(letter + m[2].str());
        if (!pc) continue;
        const bool minor = mode == "m" || mode == "min" || mode == "minor";
        return Key{*pc, minor ? ScaleType::NaturalMinor : ScaleType::Major};
    }
    return std::nullopt;
}

double BeatFileInfo::suggestedBpm() const {
    if (bpmFromName) return *bpmFromName;
    return bpmConfidence >= 0.5 ? bpmDetected : 0.0;
}

std::optional<Key> BeatFileInfo::suggestedKey() const {
    if (keyFromName) return keyFromName;
    if (keyRoot >= 0 && keyConfidence >= 0.6) return Key{keyRoot, keyMinor ? ScaleType::NaturalMinor : ScaleType::Major};
    return std::nullopt;
}

bool isImportableAudio(const fs::path& file) {
    std::string e = file.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".mp3" || e == ".wav" || e == ".flac" || e == ".ogg" || e == ".aif" || e == ".aiff";
}

BeatFileInfo analyzeBeatFile(const fs::path& file) {
    BeatFileInfo info;
    const std::string stem = file.stem().string();
    info.bpmFromName = bpmFromFileName(stem);
    info.keyFromName = keyFromFileName(stem);
    AudioData d;
    if (!readAudioFile(file, d, &info.error)) return info;
    if (d.numFrames <= 0 || d.channels.empty()) {
        info.error = "the file contains no audio";
        return info;
    }
    info.sampleRate = d.sampleRate;
    info.channels = d.numChannels;
    info.seconds = static_cast<double>(d.numFrames) / d.sampleRate;
    // analyse at most 60 s from the middle (intro tags / fades are not typical of the song)
    sampler::Channels part;
    const int64_t maxFrames = static_cast<int64_t>(60 * d.sampleRate);
    const int64_t start = d.numFrames > maxFrames ? (d.numFrames - maxFrames) / 2 : 0;
    const int64_t n = std::min(d.numFrames, maxFrames);
    for (auto& c : d.channels) part.emplace_back(c.begin() + start, c.begin() + start + n);
    const auto s = sampler::analyzeSample(part, d.sampleRate);
    info.bpmDetected = s.bpm;
    info.bpmConfidence = s.bpmConfidence;
    info.keyRoot = s.keyRoot;
    info.keyMinor = s.keyMinor;
    info.keyConfidence = s.keyConfidence;
    info.ok = true;
    return info;
}

} // namespace roy::beatimport
