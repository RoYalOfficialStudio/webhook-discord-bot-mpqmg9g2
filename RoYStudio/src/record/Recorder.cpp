#include "record/Recorder.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Math.h"
#include "project/ProjectIO.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>

namespace roy {

namespace fs = std::filesystem;
namespace {
constexpr int64_t kNotRolling = std::numeric_limits<int64_t>::min();
}

Recorder::Recorder() { diskThread_ = std::thread([this] { diskLoop(); }); }

Recorder::~Recorder() {
    stopRecording();
    quit_.store(true);
    diskCv_.notify_all();
    if (diskThread_.joinable()) diskThread_.join();
    std::lock_guard lock(diskMutex_);
    Config* cfg = config_.exchange(nullptr);
    if (cfg) drain(*cfg, true);
    owned_.reset();
}

void Recorder::prepare(double sampleRate, int maxBlockSize) {
    sr_ = sampleRate;
    maxBlock_ = maxBlockSize;
}

void Recorder::setNeverLoseSeconds(double seconds) { neverLoseSeconds_ = std::clamp(seconds, 0.0, 3600.0); }

void Recorder::retire(std::unique_ptr<Config> old) {
    if (!old) return;
    // Wait until the audio thread is guaranteed not to use the old config.
    const uint64_t c0 = callbacks_.load();
    for (int i = 0; i < 2000 && inCallback_.load() && callbacks_.load() == c0; ++i)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    std::lock_guard lock(diskMutex_);
    drain(*old, true);
}

void Recorder::setTracks(const std::vector<RecordTrackConfig>& tracks) {
    if (recording_.load()) {
        log::warn("record", "track configuration change ignored while recording");
        return;
    }
    auto cfg = std::make_unique<Config>();
    size_t totalChannels = 0;
    for (auto& t : tracks) totalChannels += t.inputRight >= 0 ? 2 : 1;
    const size_t perChannelCap = totalChannels ? kMaxNeverLoseBytes / (sizeof(float) * totalChannels) : 0;
    for (auto& t : tracks) {
        auto ts = std::make_unique<TrackState>();
        ts->cfg = t;
        ts->channels = t.inputRight >= 0 ? 2 : 1;
        ts->gain = dbToGain(t.inputGainDb);
        ts->fifo = std::make_unique<SampleFifo>(static_cast<size_t>(sr_ * 4.0) * static_cast<size_t>(ts->channels)); // 4 s disk slack
        ts->markers = std::make_unique<SpscQueue<Marker>>(1024);
        ts->ringSize = std::min(perChannelCap, static_cast<size_t>(neverLoseSeconds_ * sr_));
        if (ts->ringSize > 0) {
            ts->ring.assign(static_cast<size_t>(ts->channels), std::vector<float>(ts->ringSize, 0.0f));
            ts->markCount = ts->ringSize / 32 + 1024;
            ts->markPos = std::make_unique<std::atomic<uint64_t>[]>(ts->markCount);
            ts->markTimeline = std::make_unique<std::atomic<int64_t>[]>(ts->markCount);
        }
        cfg->tracks.push_back(std::move(ts));
    }
    Config* raw = cfg.get();
    config_.exchange(raw);
    std::unique_ptr<Config> old = std::move(owned_);
    owned_ = std::move(cfg);
    retire(std::move(old));
}

void Recorder::setPunch(bool enabled, int64_t in, int64_t out) {
    punchIn_.store(std::min(in, out));
    punchOut_.store(std::max(in, out));
    punchOn_.store(enabled && out != in);
}

bool Recorder::startRecording(std::string* error) {
    if (folder_.empty()) {
        if (error) *error = "no recording folder set";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (ec) {
        if (error) *error = "cannot create recording folder: " + ec.message();
        return false;
    }
    Config* cfg = config_.load();
    if (!cfg || cfg->tracks.empty()) {
        if (error) *error = "no armed tracks";
        return false;
    }
    stopRequested_.store(false);
    recording_.store(true);
    log::info("record", "recording armed on {} track(s)", cfg->tracks.size());
    return true;
}

void Recorder::stopRecording() {
    if (!recording_.exchange(false)) return;
    // Let the audio thread close open takes (it sees recording_ == false).
    const uint64_t c0 = callbacks_.load();
    for (int i = 0; i < 300 && callbacks_.load() < c0 + 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (callbacks_.load() < c0 + 2) forceClose_.store(true); // audio thread idle: disk thread closes takes
    flush();
    log::info("record", "recording stopped");
}

void Recorder::flush() {
    const uint64_t req = flushRequest_.fetch_add(1) + 1;
    diskCv_.notify_all();
    for (int i = 0; i < 5000 && flushDone_.load() < req; ++i) {
        diskCv_.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Recorder::TrackState* Recorder::find(const std::string& trackId) const {
    Config* cfg = config_.load();
    if (!cfg) return nullptr;
    for (auto& t : cfg->tracks)
        if (t->cfg.trackId == trackId) return t.get();
    return nullptr;
}

float Recorder::inputPeak(const std::string& trackId, bool reset) {
    TrackState* t = find(trackId);
    if (!t) return 0.0f;
    return reset ? t->peak.exchange(0.0f) : t->peak.load();
}

uint32_t Recorder::clipCount(const std::string& trackId) const {
    TrackState* t = find(trackId);
    return t ? t->clips.load() : 0;
}

std::vector<FinishedTake> Recorder::collectFinishedTakes() {
    std::lock_guard lock(finishedMutex_);
    return std::exchange(finished_, {});
}

// ------------------------------------------------------------------ audio thread
void Recorder::onAudioInput(const float* const* inputs, int numInputs, int numFrames, const Transport::Segment* segs,
                            int numSegs) noexcept {
    inCallback_.store(true);
    Config* cfg = config_.load();
    if (cfg) {
        const bool rec = recording_.load(std::memory_order_acquire);
        const bool punch = punchOn_.load(std::memory_order_relaxed);
        const int64_t pIn = punchIn_.load(std::memory_order_relaxed), pOut = punchOut_.load(std::memory_order_relaxed);
        for (auto& tsPtr : cfg->tracks) {
            TrackState& ts = *tsPtr;
            const int li = ts.cfg.inputLeft, ri = ts.cfg.inputRight;
            if (li < 0 || li >= numInputs) continue;
            const float* L = inputs[li];
            const float* R = (ri >= 0 && ri < numInputs) ? inputs[ri] : nullptr;
            const int nCh = ts.channels;

            // meters + clip detection (clipping is judged on the raw converter signal)
            float pk = 0.0f;
            uint32_t clipped = 0;
            for (int i = 0; i < numFrames; ++i) {
                const float a = std::fabs(L[i]);
                const float b = R ? std::fabs(R[i]) : 0.0f;
                if (a >= 0.999f || b >= 0.999f) ++clipped;
                pk = std::max(pk, std::max(a, b) * ts.gain);
            }
            if (pk > ts.peak.load(std::memory_order_relaxed)) ts.peak.store(pk, std::memory_order_relaxed);
            if (clipped) {
                ts.clips.fetch_add(clipped, std::memory_order_relaxed);
                if (ts.takeOpen) ts.takeClips.fetch_add(clipped, std::memory_order_relaxed);
                log::audioEvent(log::Level::Warn, "input clipping detected", static_cast<double>(clipped));
            }

            // never-lose ring (always, while configured)
            if (ts.ringSize > 0) {
                const uint64_t w = ts.ringWritten.load(std::memory_order_relaxed);
                for (int s = 0; s < numSegs && ts.markCount; ++s) {
                    const uint64_t idx = ts.markWritten.load(std::memory_order_relaxed);
                    ts.markPos[idx % ts.markCount].store(w + static_cast<uint64_t>(segs[s].blockOffset), std::memory_order_relaxed);
                    ts.markTimeline[idx % ts.markCount].store(segs[s].rolling ? segs[s].timelineStart : kNotRolling,
                                                              std::memory_order_relaxed);
                    ts.markWritten.store(idx + 1, std::memory_order_release);
                }
                for (int i = 0; i < numFrames; ++i) {
                    const size_t pos = static_cast<size_t>((w + static_cast<uint64_t>(i)) % ts.ringSize);
                    ts.ring[0][pos] = L[i] * ts.gain;
                    if (nCh > 1) ts.ring[1][pos] = (R ? R[i] : L[i]) * ts.gain;
                }
                ts.ringWritten.store(w + static_cast<uint64_t>(numFrames), std::memory_order_release);
            }

            // recording
            for (int s = 0; s < numSegs; ++s) {
                const auto& seg = segs[s];
                auto endTake = [&] {
                    if (!ts.takeOpen) return;
                    Marker m;
                    m.type = Marker::End;
                    m.samplePos = ts.framesPushed;
                    if (!ts.markers->push(m)) dropped_.fetch_add(1, std::memory_order_relaxed);
                    ts.takeOpen = false;
                };
                if (!rec || !seg.rolling) {
                    endTake();
                    continue;
                }
                if (seg.jumped && ts.takeOpen) {
                    endTake();
                    ++ts.loopPass;
                }
                int64_t a = seg.timelineStart, b = seg.timelineStart + seg.numFrames;
                if (punch) {
                    a = std::max(a, pIn);
                    b = std::min(b, pOut);
                    if (b <= a) {
                        if (seg.timelineStart >= pOut) endTake();
                        continue;
                    }
                }
                if (!ts.takeOpen) {
                    Marker m;
                    m.type = Marker::Start;
                    m.samplePos = ts.framesPushed;
                    m.timeline = a;
                    m.loopPass = ts.loopPass;
                    if (!ts.markers->push(m)) {
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    ts.takeOpen = true;
                    ts.takeClips.store(0, std::memory_order_relaxed);
                }
                const int from = seg.blockOffset + static_cast<int>(a - seg.timelineStart);
                const int count = static_cast<int>(b - a);
                float chunk[512];
                const int framesPerChunk = 512 / nCh;
                for (int done = 0; done < count;) {
                    const int n = std::min(framesPerChunk, count - done);
                    for (int i = 0; i < n; ++i) {
                        const int k = from + done + i;
                        chunk[i * nCh] = L[k] * ts.gain;
                        if (nCh > 1) chunk[i * nCh + 1] = (R ? R[k] : L[k]) * ts.gain;
                    }
                    const size_t want = static_cast<size_t>(n * nCh);
                    const size_t wrote = ts.fifo->write(chunk, want);
                    if (wrote < want) {
                        // Disk too slow: pad the stream position so timing stays correct.
                        dropped_.fetch_add(want - wrote, std::memory_order_relaxed);
                        log::audioEvent(log::Level::Error, "recording FIFO overflow (samples dropped)", static_cast<double>(want - wrote));
                    }
                    ts.framesPushed += wrote / static_cast<size_t>(nCh);
                    done += n;
                }
                if (punch && b == pOut) endTake();
            }
            if (!rec && ts.takeOpen) {
                Marker m;
                m.type = Marker::End;
                m.samplePos = ts.framesPushed;
                if (ts.markers->push(m)) ts.takeOpen = false;
            }
            if (!rec) ts.loopPass = 0;
        }
    }
    callbacks_.fetch_add(1);
    inCallback_.store(false);
    if (cfg && recording_.load(std::memory_order_relaxed)) diskCv_.notify_one();
}

// ------------------------------------------------------------------ disk thread
void Recorder::diskLoop() {
    while (!quit_.load()) {
        {
            std::unique_lock lock(diskMutex_);
            diskCv_.wait_for(lock, std::chrono::milliseconds(10));
            const uint64_t req = flushRequest_.load();
            if (Config* cfg = config_.load()) drain(*cfg, forceClose_.exchange(false));
            flushDone_.store(req);
        }
    }
}

void Recorder::drain(Config& cfg, bool final) {
    for (auto& tsPtr : cfg.tracks) {
        TrackState& ts = *tsPtr;
        const int nCh = ts.channels;
        std::vector<float> buf;
        for (;;) {
            if (!ts.hasPendingMarker && ts.markers->pop(ts.pendingMarker)) ts.hasPendingMarker = true;
            if (ts.hasPendingMarker && ts.framesDrained == ts.pendingMarker.samplePos) {
                const Marker m = ts.pendingMarker;
                ts.hasPendingMarker = false;
                if (m.type == Marker::Start) {
                    if (ts.writing) {
                        ts.writer.close();
                        ts.current.frames = ts.writer.framesWritten();
                        std::lock_guard l(finishedMutex_);
                        finished_.push_back(ts.current);
                    }
                    ++ts.takeCounter;
                    const std::string base = std::format("{}_Take{:02d}_{}.wav", sanitizeFileName(ts.cfg.trackName),
                                                         ts.takeCounter, files::nowCompact());
                    const fs::path path = files::uniquePath(folder_ / base);
                    std::string err;
                    if (ts.writer.open(path, sr_, nCh, format_, false, &err)) {
                        ts.writing = true;
                        ts.current = FinishedTake{};
                        ts.current.trackId = ts.cfg.trackId;
                        ts.current.path = path;
                        ts.current.timelineStart = m.timeline - latency_.load();
                        ts.current.loopPass = m.loopPass;
                        ts.current.channels = nCh;
                        ts.current.sampleRate = sr_;
                    } else {
                        ts.writing = false;
                        log::error("record", "cannot create take file: {}", err);
                    }
                } else if (ts.writing) {
                    ts.writer.close();
                    ts.writing = false;
                    ts.current.frames = ts.writer.framesWritten();
                    ts.current.clippedSamples = ts.takeClips.load();
                    std::lock_guard l(finishedMutex_);
                    if (ts.current.frames > 0) finished_.push_back(ts.current);
                    else {
                        std::error_code ec;
                        fs::remove(ts.current.path, ec); // our own empty file
                    }
                }
                continue;
            }
            uint64_t limit = ts.hasPendingMarker ? ts.pendingMarker.samplePos - ts.framesDrained : UINT64_MAX;
            const uint64_t avail = ts.fifo->available() / static_cast<size_t>(nCh);
            const uint64_t n = std::min<uint64_t>({avail, limit, 16384});
            if (n == 0) break;
            buf.resize(static_cast<size_t>(n) * static_cast<size_t>(nCh));
            ts.fifo->read(buf.data(), buf.size());
            if (ts.writing) ts.writer.writeInterleaved(buf.data(), static_cast<int>(n));
            ts.framesDrained += n;
        }
        if (final && ts.writing && !ts.hasPendingMarker) {
            ts.writer.close();
            ts.writing = false;
            ts.current.frames = ts.writer.framesWritten();
            std::lock_guard l(finishedMutex_);
            if (ts.current.frames > 0) finished_.push_back(ts.current);
        }
    }
}

// ------------------------------------------------------------------ never lose
double Recorder::neverLoseSecondsAvailable(const std::string& trackId) const {
    TrackState* t = find(trackId);
    if (!t || t->ringSize == 0) return 0.0;
    return static_cast<double>(std::min<uint64_t>(t->ringWritten.load(), t->ringSize)) / sr_;
}

std::optional<RecoveredPerformance> Recorder::recoverLastPerformance(const std::string& trackId, bool trim,
                                                                     std::string* error) {
    TrackState* t = find(trackId);
    if (!t || t->ringSize == 0) {
        if (error) *error = "track is not armed / never-lose buffer disabled";
        return std::nullopt;
    }
    const uint64_t W = t->ringWritten.load(std::memory_order_acquire);
    const uint64_t avail = std::min<uint64_t>(W, t->ringSize);
    if (avail == 0) {
        if (error) *error = "nothing captured yet";
        return std::nullopt;
    }
    uint64_t start = W - avail;
    std::vector<std::vector<float>> data(static_cast<size_t>(t->channels), std::vector<float>(static_cast<size_t>(avail)));
    for (int c = 0; c < t->channels; ++c)
        for (uint64_t i = 0; i < avail; ++i) data[static_cast<size_t>(c)][static_cast<size_t>(i)] = t->ring[static_cast<size_t>(c)][static_cast<size_t>((start + i) % t->ringSize)];
    // Samples overwritten by the audio thread while copying are dropped.
    const uint64_t W2 = t->ringWritten.load(std::memory_order_acquire);
    uint64_t skip = W2 > W ? std::min<uint64_t>(avail, W2 - W + static_cast<uint64_t>(maxBlock_)) : 0;
    uint64_t s = skip, e = avail;

    if (trim) {
        // Find the last performance: 10 ms windows above -50 dBFS, gaps < 3 s join.
        const uint64_t win = static_cast<uint64_t>(sr_ * 0.01);
        const double thr = dbToGain(-50.0);
        int64_t lastActive = -1, firstActive = -1;
        const uint64_t nWin = (avail - s) / win;
        std::vector<bool> active(static_cast<size_t>(nWin));
        for (uint64_t k = 0; k < nWin; ++k) {
            double acc = 0;
            for (uint64_t i = s + k * win; i < s + (k + 1) * win; ++i)
                for (auto& ch : data) acc += static_cast<double>(ch[static_cast<size_t>(i)]) * ch[static_cast<size_t>(i)];
            active[static_cast<size_t>(k)] = std::sqrt(acc / static_cast<double>(win * data.size())) > thr;
        }
        for (int64_t k = static_cast<int64_t>(nWin) - 1; k >= 0; --k)
            if (active[static_cast<size_t>(k)]) { lastActive = k; break; }
        if (lastActive < 0) {
            if (error) *error = "no performance found in the never-lose buffer (only silence)";
            return std::nullopt;
        }
        const int64_t maxGap = static_cast<int64_t>(3.0 / 0.01);
        firstActive = lastActive;
        int64_t gap = 0;
        for (int64_t k = lastActive; k >= 0; --k) {
            if (active[static_cast<size_t>(k)]) {
                firstActive = k;
                gap = 0;
            } else if (++gap > maxGap) {
                break;
            }
        }
        const uint64_t pad = static_cast<uint64_t>(sr_ * 0.25);
        const uint64_t ns = s + static_cast<uint64_t>(firstActive) * win;
        const uint64_t ne = s + static_cast<uint64_t>(lastActive + 1) * win;
        s = ns > s + pad ? ns - pad : s;
        e = std::min(avail, ne + pad);
    }
    std::vector<std::vector<float>> out(data.size());
    for (size_t c = 0; c < data.size(); ++c) out[c].assign(data[c].begin() + static_cast<long>(s), data[c].begin() + static_cast<long>(e));

    RecoveredPerformance r;
    r.trackId = trackId;
    r.channels = t->channels;
    r.frames = static_cast<int64_t>(e - s);
    const uint64_t absStart = start + s;
    r.secondsBeforeNow = static_cast<double>(W - absStart) / sr_;
    // Timeline position from the newest block mark at or before absStart.
    const uint64_t marks = t->markWritten.load(std::memory_order_acquire);
    const uint64_t firstMark = marks > t->markCount ? marks - t->markCount : 0;
    for (uint64_t k = marks; k > firstMark; --k) {
        const uint64_t idx = (k - 1) % t->markCount;
        const uint64_t pos = t->markPos[idx].load(std::memory_order_relaxed);
        if (pos <= absStart) {
            const int64_t tl = t->markTimeline[idx].load(std::memory_order_relaxed);
            if (tl != kNotRolling) {
                r.hasTimeline = true;
                r.timelineStart = tl + static_cast<int64_t>(absStart - pos) - latency_.load();
            }
            break;
        }
    }
    const fs::path path = files::uniquePath(folder_ / std::format("Recovered_{}_{}.wav", sanitizeFileName(t->cfg.trackName), files::nowCompact()));
    std::string err;
    if (!writeWavFile(path, out, sr_, format_, false, &err)) {
        if (error) *error = err;
        return std::nullopt;
    }
    r.path = path;
    log::info("record", "recovered {:.2f} s of performance on '{}' -> {}", static_cast<double>(r.frames) / sr_, t->cfg.trackName, path.string());
    return r;
}

} // namespace roy
