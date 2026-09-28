#include "browser/SamplePreview.h"
#include "core/Files.h"
#include "dsp/Analysis.h"
#include "dsp/TimeStretch.h"
#include "io/AudioFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace roy::browser {

double bpmFromFileName(const std::string& name) {
    std::string s = name;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::regex withBpm(R"((\d{2,3}(?:\.\d+)?)\s*[-_ ]?\s*bpm)");
    static const std::regex bpmFirst(R"(bpm\s*[-_ ]?\s*(\d{2,3}))");
    static const std::regex bare(R"((?:^|[_\- ])(\d{2,3})(?:[_\- .]|$))");
    std::smatch m;
    for (const auto* re : {&withBpm, &bpmFirst, &bare})
        if (std::regex_search(s, m, *re)) {
            const double v = std::stod(m[1].str());
            if (v >= 60 && v <= 220) return v;
        }
    return 0;
}

bool looksLikeLoop(const std::string& name, double seconds, double bpm) {
    std::string s = name;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.find("loop") != std::string::npos) return true;
    if (bpm > 0 && seconds > 1.0) {
        const double beats = seconds * bpm / 60.0;
        const double bars = beats / 4.0;
        return std::fabs(bars - std::round(bars)) < 0.03 && std::round(bars) >= 1;
    }
    return false;
}

PreviewInfo Previewer::preview(const fs::path& file, const PreviewOptions& o) {
    PreviewInfo info;
    const double sr = engine_.sampleRate();
    Cached* c = nullptr;
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->path == file) {
            cache_.splice(cache_.begin(), cache_, it);
            c = &cache_.front();
            break;
        }
    if (!c) {
        std::string err;
        auto d = loadAudioAt(file, sr, &err);
        if (!d) {
            info.error = err.empty() ? "cannot decode " + file.string() : err;
            return info;
        }
        double bpm = bpmFromFileName(file.stem().string());
        std::string src = bpm > 0 ? "filename" : "";
        if (bpm <= 0 && d->numFrames > static_cast<int64_t>(2 * sr)) {
            std::vector<float> mono(static_cast<size_t>(d->numFrames));
            for (int64_t i = 0; i < d->numFrames; ++i) {
                float v = 0;
                for (auto& ch : d->channels) v += ch[static_cast<size_t>(i)];
                mono[static_cast<size_t>(i)] = v / static_cast<float>(d->channels.size());
            }
            const auto est = dsp::estimateBpm(mono.data(), d->numFrames, sr);
            if (est.confidence > 0.3) {
                bpm = est.bpm;
                src = "analysis";
            }
        }
        cache_.push_front({file, d, bpm, src});
        while (cache_.size() > 16) cache_.pop_back();
        c = &cache_.front();
    }
    std::shared_ptr<const AudioData> play = c->data;
    info.seconds = static_cast<double>(c->data->numFrames) / sr;
    info.sourceBpm = c->bpm;
    info.bpmSource = c->bpmSource;
    info.isLoop = looksLikeLoop(file.stem().string(), info.seconds, c->bpm);
    if (o.tempo == TempoMode::Project && c->bpm > 0 && o.projectBpm > 0 && std::fabs(c->bpm - o.projectBpm) > 0.01) {
        info.stretch = c->bpm / o.projectBpm; // >1 = longer (slower)
        auto st = std::make_shared<AudioData>(*c->data);
        st->channels = dsp::timeStretch(c->data->channels, info.stretch, sr);
        st->numFrames = st->channels.empty() ? 0 : static_cast<int64_t>(st->channels[0].size());
        play = st;
    }
    current_ = file;
    engine_.preview().play(play, o.gain, o.loop || info.isLoop, engine_.processedBlocks());
    info.ok = true;
    return info;
}

void Previewer::stop() {
    engine_.preview().stop();
    current_.clear();
}

std::vector<std::string> categories() { return {"Samples", "Drums", "808", "Loops", "Vocals"}; }

fs::path categoryFolder(const std::string& category) {
    const fs::path p = files::userDataDirectory() / "Library" / category;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

} // namespace roy::browser
