#include "export/Exporter.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Math.h"
#include "dsp/Resampler.h"
#include "export/FlacEncoder.h"
#include "io/AudioFile.h"
#include "project/ProjectIO.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace roy::exporting {

namespace fs = std::filesystem;

const char* extensionFor(Format f) {
    switch (f) {
    case Format::Wav: return ".wav";
    case Format::Flac: return ".flac";
    case Format::Mp3: return ".mp3";
    }
    return ".wav";
}

std::vector<std::vector<int32_t>> quantize(const std::vector<std::vector<float>>& in, int bits, Dither dither, uint64_t seed) {
    const double scale = std::ldexp(1.0, bits - 1) - 1.0;
    const int64_t maxV = static_cast<int64_t>(scale), minV = -maxV - 1;
    Rng rng(seed);
    std::vector<std::vector<int32_t>> out(in.size());
    for (size_t c = 0; c < in.size(); ++c) {
        out[c].resize(in[c].size());
        double err = 0; // first-order noise shaping state
        for (size_t i = 0; i < in[c].size(); ++i) {
            double v = static_cast<double>(std::isfinite(in[c][i]) ? in[c][i] : 0.0f) * scale;
            if (dither != Dither::None) {
                if (dither == Dither::TpdfShaped) v -= err;
                const double d = rng.uniform(-0.5, 0.5) + rng.uniform(-0.5, 0.5); // TPDF, +-1 LSB
                const double q = std::round(v + d);
                if (dither == Dither::TpdfShaped) err = q - v;
                v = q;
            } else {
                v = std::round(v);
            }
            out[c][i] = static_cast<int32_t>(std::clamp<int64_t>(static_cast<int64_t>(v), minV, maxV));
        }
    }
    return out;
}

bool writeAudio(const fs::path& path, const std::vector<std::vector<float>>& audio, double sr, Format fmt, int bitDepth, Dither dither,
                std::string* error, const mp3::Options* mp3opt) {
    switch (fmt) {
    case Format::Mp3: {
        const mp3::Options def;
        return mp3::encode(path, audio, static_cast<int>(std::lround(sr)), mp3opt ? *mp3opt : def, error);
    }
    case Format::Flac: {
        const int bits = bitDepth >= 24 ? 24 : 16;
        auto q = quantize(audio, bits, dither);
        return flac::encode(path, q, static_cast<int>(std::lround(sr)), bits, {}, error);
    }
    case Format::Wav: {
        if (bitDepth >= 32) return writeWavFile(path, audio, sr, SampleFormat::Float32, false, error);
        // integer: dither + quantize, then write exact integer values
        const int bits = bitDepth >= 24 ? 24 : 16;
        auto q = quantize(audio, bits, dither);
        const double scale = std::ldexp(1.0, bits - 1) - 1.0;
        std::vector<std::vector<float>> back(q.size());
        for (size_t c = 0; c < q.size(); ++c) {
            back[c].resize(q[c].size());
            for (size_t i = 0; i < q[c].size(); ++i) back[c][i] = static_cast<float>(q[c][i] / scale);
        }
        return writeWavFile(path, back, sr, bits == 24 ? SampleFormat::Pcm24 : SampleFormat::Pcm16, false, error);
    }
    }
    return false;
}

double automaticTailSeconds(ProjectRuntime& runtime, const Project& project) {
    double tail = 0.0;
    auto consider = [&](const PluginSlot& s) {
        if (s.bypass) return;
        if (auto p = runtime.processorForSlot(s.id)) tail = std::max(tail, p->tailSeconds());
    };
    for (auto& c : project.channels)
        for (auto& s : c.inserts) consider(s);
    for (auto& t : project.tracks)
        if (t.instrument) consider(*t.instrument);
    return std::min(10.0, tail + 0.25);
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool isVocalTrack(const Track& t) {
    const std::string r = lower(t.role + " " + t.name);
    return r.find("vocal") != std::string::npos || r.find("vox") != std::string::npos || r.find("adlib") != std::string::npos ||
           r.find("double") != std::string::npos || r.find("harmony") != std::string::npos;
}

struct Job {
    std::string what;
    std::vector<std::vector<float>> audio;
};

} // namespace

ExportResult exportProject(AudioEngine& engine, ProjectRuntime& runtime, Project& project, const ExportOptions& o) {
    ExportResult res;
    if (o.format == Format::Mp3) {
        std::string why;
        if (!mp3::available(&why)) {
            res.error = "MP3 export is not available: " + why;
            return res;
        }
        if (!o.mp3.vbr && !mp3::validBitrate(o.mp3.bitrateKbps)) {
            res.error = std::format("invalid MP3 bitrate {} kbps", o.mp3.bitrateKbps);
            return res;
        }
    }
    if (o.folder.empty()) {
        res.error = "no export folder";
        return res;
    }
    if (!runtime.rebuild(project)) {
        res.error = "project could not be compiled for rendering";
        return res;
    }
    const double sr = engine.sampleRate();
    double startBeat = 0, endBeat = project.endBeat();
    if (o.range == Range::Selection) {
        startBeat = o.startBeat;
        endBeat = o.endBeat;
    } else if (o.range == Range::Loop) {
        startBeat = project.loop.startBeat;
        endBeat = project.loop.endBeat;
    }
    if (endBeat <= startBeat) {
        res.error = "empty export range";
        return res;
    }
    const double tail = o.tailSeconds >= 0 ? o.tailSeconds : automaticTailSeconds(runtime, project);
    const int64_t s0 = static_cast<int64_t>(std::llround(project.tempo.beatToSample(startBeat, sr)));
    const int64_t s1 = static_cast<int64_t>(std::llround(project.tempo.beatToSample(endBeat, sr) + tail * sr));
    // Plugin delay compensation: render `latency` extra samples and drop them from the front.
    const int latency = runtime.graphLatencySamples();

    // which channels to capture
    std::vector<std::pair<std::string, std::string>> captureList; // (what, channelId)
    if (o.stems == Stems::AllTracks || o.stems == Stems::SelectedTracks || o.stems == Stems::VocalStems)
        for (auto& t : project.tracks) {
            if (o.stems == Stems::SelectedTracks &&
                std::find(o.selectedTrackIds.begin(), o.selectedTrackIds.end(), t.id) == o.selectedTrackIds.end())
                continue;
            if (o.stems == Stems::VocalStems && !isVocalTrack(t)) continue;
            captureList.push_back({t.name, t.channelId});
        }
    if (o.stems == Stems::MixerBusses)
        for (auto& c : project.channels)
            if (c.kind == ChannelKind::Bus) captureList.push_back({c.name, c.id});

    ChannelCapture cap;
    std::vector<std::string> ids;
    for (auto& [w, id] : captureList) ids.push_back(id);
    const int64_t frames = s1 - s0 + latency;
    cap.allocate(ids, frames);
    OfflineRenderOptions ro;
    ro.startSample = s0;
    ro.numFrames = frames;
    ro.capture = ids.empty() ? nullptr : &cap;
    ro.progress = o.progress;
    std::string err;
    auto mix = renderOffline(engine, ro, &err);
    if (mix.empty()) {
        res.error = "render failed: " + err;
        return res;
    }
    auto trim = [&](std::vector<std::vector<float>> a) {
        for (auto& c : a) c.erase(c.begin(), c.begin() + std::min<int64_t>(latency, static_cast<int64_t>(c.size())));
        return a;
    };
    std::vector<Job> jobs;
    if (o.mixdown) jobs.push_back({"mixdown", trim(mix)});
    for (size_t k = 0; k < captureList.size(); ++k) {
        // Captured stems are taken before the delay compensation of their mixer path:
        // shift each by (total latency - its own arrival latency) so stems line up with the mixdown.
        auto stem = std::move(cap.data[k]);
        const int shift = std::max(0, latency - runtime.channelArrivalLatency(captureList[k].second));
        if (shift > 0)
            for (auto& c : stem) {
                c.insert(c.begin(), static_cast<size_t>(shift), 0.0f);
                c.resize(static_cast<size_t>(frames));
            }
        jobs.push_back({captureList[k].first, trim(std::move(stem))});
    }
    if (o.stems == Stems::Instrumental) {
        // second pass with all vocal tracks muted (model restored afterwards)
        std::vector<std::pair<MixerChannel*, bool>> saved;
        for (auto& t : project.tracks)
            if (isVocalTrack(t))
                if (auto* ch = project.findChannel(t.channelId)) {
                    saved.push_back({ch, ch->mute});
                    ch->mute = true;
                }
        runtime.syncParams(project);
        auto inst = renderOffline(engine, ro, &err);
        for (auto& [ch, m] : saved) ch->mute = m;
        runtime.syncParams(project);
        if (inst.empty()) {
            res.error = "instrumental render failed: " + err;
            return res;
        }
        jobs.push_back({"Instrumental", trim(inst)});
    }

    double outRate = o.sampleRate > 0 ? o.sampleRate : sr;
    if (o.format == Format::Mp3 && !mp3::supportedSampleRate(static_cast<int>(std::lround(outRate)))) {
        const double to = outRate > 44100.5 ? 48000.0 : outRate > 32000.5 ? 44100.0 : 32000.0;
        res.warnings.push_back(std::format("MP3 cannot store {:.0f} Hz - resampled to {:.0f} Hz", outRate, to));
        outRate = to;
    }
    std::error_code ec;
    fs::create_directories(o.folder, ec);
    const std::string base = sanitizeFileName(o.baseName.empty() ? project.name : o.baseName);
    for (auto& job : jobs) {
        auto audio = std::move(job.audio);
        if (std::fabs(outRate - sr) > 0.5)
            for (auto& c : audio) c = dsp::resample(c, sr, outRate);
        auto stats = dsp::measureLoudness(audio, outRate);
        double gainDb = 0.0;
        if (o.normalize == Normalize::Peak && stats.samplePeakDb > -120) gainDb = o.normalizePeakDb - stats.samplePeakDb;
        if (o.normalize == Normalize::Loudness && stats.integratedLufs > -70) {
            gainDb = dsp::normalizationGainDb(stats, o.normalizeLufs, o.truePeakCeilingDb);
            if (std::fabs(o.normalizeLufs - stats.integratedLufs - gainDb) > 0.05)
                res.warnings.push_back(std::format("{}: loudness target {:.1f} LUFS not reached without exceeding {:.1f} dBTP "
                                                   "(reached {:.1f} LUFS) - use a limiter for more loudness",
                                                   job.what, o.normalizeLufs, o.truePeakCeilingDb, stats.integratedLufs + gainDb));
        }
        if (gainDb != 0.0) {
            const float g = static_cast<float>(dbToGain(gainDb));
            for (auto& c : audio)
                for (auto& v : c) v *= g;
            stats = dsp::measureLoudness(audio, outRate);
        }
        if ((o.bitDepth < 32 || o.format != Format::Wav) && stats.samplePeakDb > 0.0)
            res.warnings.push_back(std::format("{}: peaks at {:+.2f} dBFS will clip in {}-bit output", job.what, stats.samplePeakDb, o.bitDepth));
        const std::string name = job.what == "mixdown" ? base : std::format("{}_{}", base, sanitizeFileName(job.what));
        const fs::path path = files::uniquePath(o.folder / (name + extensionFor(o.format)));
        if (!writeAudio(path, audio, outRate, o.format, o.bitDepth, o.dither, &err, &o.mp3)) {
            res.error = std::format("writing {} failed: {}", path.string(), err);
            return res;
        }
        res.files.push_back({path, job.what, stats, gainDb});
        log::info("export", "exported {} ({:.1f} LUFS, {:.1f} dBTP)", path.string(), stats.integratedLufs, stats.truePeakDb);
    }
    res.renderedSeconds = static_cast<double>(s1 - s0) / sr;
    res.ok = true;
    return res;
}

} // namespace roy::exporting
