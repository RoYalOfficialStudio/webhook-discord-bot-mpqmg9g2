#include "audio/AudioEngine.h"
#include "project/AutomationShape.h"
#include "core/Log.h"
#include "core/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#define ROY_CPU_RELAX() _mm_pause()
#else
#define ROY_CPU_RELAX() std::this_thread::yield()
#endif

namespace roy {

float AutomationCurve::valueAt(int64_t t) const noexcept {
    if (points.empty()) return 0.0f;
    if (t <= points.front().sample) return points.front().value;
    if (t >= points.back().sample) return points.back().value;
    auto it = std::upper_bound(points.begin(), points.end(), t, [](int64_t v, const Point& p) { return v < p.sample; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = static_cast<double>(b.sample - a.sample);
    const double x = span > 0 ? static_cast<double>(t - a.sample) / span : 0.0;
    return static_cast<float>(automation::interpolate(a.value, b.value, a.curve, a.tension, x));
}

namespace {
inline float fadeShape(int curve, float x) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (curve) {
    case 0: return x;
    case 1: return std::sin(x * static_cast<float>(kPi) * 0.5f);
    case 2: return x * x;
    case 3: return 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * x);
    default: return x;
    }
}
} // namespace

void finalizeGraph(RenderGraph& g) {
    const int n = static_cast<int>(g.channels.size());
    for (auto& ch : g.channels) ch.incoming.clear();
    bool ordered = true;
    for (int i = 0; i < n; ++i)
        for (int o = 0; o < static_cast<int>(g.channels[static_cast<size_t>(i)].outputs.size()); ++o) {
            const int t = g.channels[static_cast<size_t>(i)].outputs[static_cast<size_t>(o)].target;
            if (t < 0 || t >= n) continue;
            if (t <= i) ordered = false;
            g.channels[static_cast<size_t>(t)].incoming.push_back({i, o}); // i ascending: fixed summing order
        }
    std::vector<int> level(static_cast<size_t>(n), 0);
    int maxLevel = 0;
    for (int i = 0; i < n; ++i) {
        const auto& ch = g.channels[static_cast<size_t>(i)];
        int lv = 0;
        for (auto& in : ch.incoming) lv = std::max(lv, level[static_cast<size_t>(in.source)] + 1);
        for (auto& ins : ch.inserts)
            if (ins.sidechainChannel >= 0) {
                if (ins.sidechainChannel >= i) ordered = false;
                else lv = std::max(lv, level[static_cast<size_t>(ins.sidechainChannel)] + 1);
            }
        level[static_cast<size_t>(i)] = lv;
        maxLevel = std::max(maxLevel, lv);
    }
    if (!ordered) { // defensive: not topologically sorted -> fully sequential schedule
        for (int i = 0; i < n; ++i) level[static_cast<size_t>(i)] = i;
        maxLevel = n - 1;
    }
    g.levelOrder.clear();
    g.levelStart.assign(1, 0);
    for (int lv = 0; lv <= maxLevel; ++lv) {
        for (int i = 0; i < n; ++i)
            if (level[static_cast<size_t>(i)] == lv) g.levelOrder.push_back(i);
        g.levelStart.push_back(static_cast<int>(g.levelOrder.size()));
    }
}

AudioEngine::AudioEngine() {
    liveIn_ = std::make_unique<SpscQueue<LiveMidiMessage>>(1024);
    liveRec_ = std::make_unique<SpscQueue<RecordedMidi>>(8192);
    prepare(48000.0, 512);
}

int AudioEngine::defaultWorkerThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(hw) - 1, 0, 7);
}

void AudioEngine::setWorkerThreads(int n) {
    n = std::clamp(n, 0, 31);
    if (n == static_cast<int>(workers_.size())) return;
    // stop existing helpers
    quitWorkers_.store(true, std::memory_order_release);
    for (auto& w : workers_) {
        w->go.fetch_add(1, std::memory_order_acq_rel);
        w->go.notify_one();
    }
    for (auto& w : workers_)
        if (w->thread.joinable()) w->thread.join();
    workers_.clear();
    quitWorkers_.store(false, std::memory_order_release);
    for (int i = 0; i < n; ++i) {
        workers_.push_back(std::make_unique<Worker>());
        workers_.back()->go.store(ticket_, std::memory_order_relaxed);
        workers_.back()->finished.store(ticket_, std::memory_order_relaxed);
    }
    // The start ticket is captured HERE, not read by the new thread: work may be posted
    // before the thread runs, and it must not mistake that ticket for "already seen".
    const uint32_t startTicket = ticket_;
    for (int i = 0; i < n; ++i) workers_[static_cast<size_t>(i)]->thread = std::thread([this, i, startTicket] { workerLoop(i, startTicket); });
}

void AudioEngine::workerLoop(int index, uint32_t startTicket) noexcept {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    sched_param sp{};
    sp.sched_priority = 70;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); // needs rtprio rights; ignored otherwise
#endif
    Worker& w = *workers_[static_cast<size_t>(index)];
    uint32_t last = startTicket;
    for (;;) {
        // Levels of one block follow each other within microseconds: spin briefly before
        // sleeping so the next level does not pay the OS wake-up latency.
        for (int spin = 0; spin < 4000 && w.go.load(std::memory_order_acquire) == last; ++spin) ROY_CPU_RELAX();
        w.go.wait(last, std::memory_order_acquire);
        const uint32_t t = w.go.load(std::memory_order_acquire);
        if (quitWorkers_.load(std::memory_order_acquire)) return;
        if (t == last) continue;
        last = t;
        {
            ScopedNoDenormals nd;
            // same partition as runLevel(): job k goes to participant k % (used + 1); 0 = audio thread
            const int used = std::min(static_cast<int>(workers_.size()), levelCount_ - 1);
            for (int k = index + 1; k < levelCount_; k += used + 1) runChannel(*levelGraph_, levelJobs_[k]);
        }
        w.finished.store(t, std::memory_order_release);
    }
}

void AudioEngine::runLevel(RenderGraph& g, int level) noexcept {
    const int* jobs = g.levelOrder.data() + g.levelStart[static_cast<size_t>(level)];
    const int count = g.levelStart[static_cast<size_t>(level) + 1] - g.levelStart[static_cast<size_t>(level)];
    const int nw = static_cast<int>(workers_.size());
    if (nw == 0 || count < 2) {
        for (int k = 0; k < count; ++k) runChannel(g, jobs[k]);
        return;
    }
    const int used = std::min(nw, count - 1);
    levelGraph_ = &g;
    levelJobs_ = jobs;
    levelCount_ = count;
    ++ticket_;
    for (int w = 0; w < used; ++w) {
        workers_[static_cast<size_t>(w)]->go.store(ticket_, std::memory_order_release);
        workers_[static_cast<size_t>(w)]->go.notify_one();
    }
    for (int k = 0; k < count; k += used + 1) runChannel(g, jobs[k]);
    for (int w = 0; w < used; ++w) {
        auto& f = workers_[static_cast<size_t>(w)]->finished;
        int spins = 0;
        while (f.load(std::memory_order_acquire) != ticket_) {
            ROY_CPU_RELAX();
            if (++spins > 20000) std::this_thread::yield();
        }
    }
}

AudioEngine::~AudioEngine() {
    setWorkerThreads(0);
    delete current_.exchange(nullptr);
    std::lock_guard lock(garbageMutex_);
    for (auto& g : garbage_) delete g.graph;
    garbage_.clear();
}

void AudioEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(16, maxBlockSize);
    metronome_.prepare(sampleRate_);
    metronomeBuf_.setSize(2, maxBlock_);
}

void AudioEngine::setGraph(std::unique_ptr<RenderGraph> graph) {
    if (graph) finalizeGraph(*graph);
    RenderGraph* old = current_.exchange(graph.release(), std::memory_order_acq_rel);
    if (old) {
        std::lock_guard lock(garbageMutex_);
        garbage_.push_back({old, blocksProcessed_.load(std::memory_order_acquire) + 1});
    }
    collectGarbage();
}

void AudioEngine::resetProcessingState() {
    if (isDeviceRunning()) return;
    RenderGraph* g = current_.load(std::memory_order_acquire);
    if (!g) return;
    for (auto& ch : g->channels) {
        if (ch.instrument) ch.instrument->reset();
        for (auto& ins : ch.inserts)
            if (ins.processor) ins.processor->reset();
        for (auto& c : ch.outputs)
            if (c.compensation) c.compensation->clear();
        ch.gainInitialised = false;
    }
    metronome_.reset();
}

void AudioEngine::collectGarbage() {
    std::lock_guard lock(garbageMutex_);
    const uint64_t done = blocksProcessed_.load(std::memory_order_acquire);
    const bool idle = !deviceRunning_.load() && !inProcess_.load();
    std::erase_if(garbage_, [&](Garbage& g) {
        if (idle || done >= g.retireAfter) {
            delete g.graph;
            return true;
        }
        return false;
    });
}

EngineStats AudioEngine::stats() const {
    EngineStats s;
    s.cpuLoad = cpuLoad_.load();
    s.peakCpuLoad = peakCpuLoad_.load();
    s.callbacks = blocksProcessed_.load();
    s.overloads = overloads_.load();
    s.nonFiniteFixes = nonFinite_.load();
    s.outputOvers = outputOvers_.load();
    return s;
}

void AudioEngine::resetStats() {
    peakCpuLoad_.store(0.0);
    overloads_.store(0);
    nonFinite_.store(0);
    outputOvers_.store(0);
}

bool AudioEngine::startInputCapture(double seconds) {
    if (capturing_.load(std::memory_order_acquire)) return false;
    capLen_ = std::max(1, static_cast<int>(seconds * sampleRate_));
    capL_.assign(static_cast<size_t>(capLen_), 0.0f);
    capR_.assign(static_cast<size_t>(capLen_), 0.0f);
    capPos_.store(0, std::memory_order_relaxed);
    capturing_.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::takeInputCapture(std::vector<float>& left, std::vector<float>& right) {
    if (capturing_.load(std::memory_order_acquire) || capLen_ == 0 || capPos_.load(std::memory_order_relaxed) < capLen_) return false;
    left = capL_;
    right = capR_;
    capLen_ = 0;
    return true;
}

float AudioEngine::inputPeak(int channel, bool reset) {
    if (channel < 0 || channel > 1) return 0.0f;
    return reset ? inputPeak_[channel].exchange(0.0f, std::memory_order_relaxed) : inputPeak_[channel].load(std::memory_order_relaxed);
}

void AudioEngine::process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                          int numFrames) noexcept {
    ScopedNoDenormals noDenormals;
    inProcess_.store(true, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();

    for (int c = 0; c < std::min(numInputs, 2); ++c) { // input level for the device check (no alloc, no lock)
        if (!inputs || !inputs[c]) continue;
        float m = 0.0f;
        for (int i = 0; i < numFrames; ++i) m = std::max(m, std::fabs(inputs[c][i]));
        if (m > inputPeak_[c].load(std::memory_order_relaxed)) inputPeak_[c].store(m, std::memory_order_relaxed);
    }
    if (capturing_.load(std::memory_order_acquire)) { // microphone test capture
        int pos = capPos_.load(std::memory_order_relaxed);
        const int n = std::min(numFrames, capLen_ - pos);
        for (int i = 0; i < n; ++i) {
            capL_[static_cast<size_t>(pos + i)] = numInputs > 0 && inputs && inputs[0] ? inputs[0][i] : 0.0f;
            capR_[static_cast<size_t>(pos + i)] = numInputs > 1 && inputs && inputs[1] ? inputs[1][i] : capL_[static_cast<size_t>(pos + i)];
        }
        pos += n;
        capPos_.store(pos, std::memory_order_relaxed);
        if (pos >= capLen_) capturing_.store(false, std::memory_order_release);
    }
    prevCbStartNs_ = cbStartNs_;
    cbStartNs_ = clock_();
    cbFrames_ = numFrames;
    RenderGraph* g = current_.load(std::memory_order_acquire);
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(maxBlock_, numFrames - done);
        processChunk(g, inputs, numInputs, outputs, numOutputs, done, n);
        done += n;
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double budget = numFrames / sampleRate_;
    const double load = budget > 0 ? elapsed / budget : 0.0;
    cpuLoad_.store(load, std::memory_order_relaxed);
    if (load > peakCpuLoad_.load(std::memory_order_relaxed)) peakCpuLoad_.store(load, std::memory_order_relaxed);
    if (load > 1.0 && deviceRunning_.load(std::memory_order_relaxed)) {
        overloads_.fetch_add(1, std::memory_order_relaxed);
        log::audioEvent(log::Level::Warn, "audio callback overload (load factor)", load);
    }
    blocksProcessed_.fetch_add(1, std::memory_order_acq_rel);
    inProcess_.store(false, std::memory_order_release);
}

void AudioEngine::processChunk(RenderGraph* g, const float* const* inputs, int numInputs, float* const* outputs,
                               int numOutputs, int outOffset, int frames) noexcept {
    Transport::Segment segs[8];
    const int numSegs = transport_.advance(frames, segs, 8);

    // Offset input pointers for this chunk (bounded, no allocation).
    const float* inPtrs[64];
    const int nIn = std::min(numInputs, 64);
    for (int c = 0; c < nIn; ++c) inPtrs[c] = inputs[c] + outOffset;

    metronomeBuf_.clear();
    float* mL = metronomeBuf_.channel(0);
    float* mR = metronomeBuf_.channel(1);

    // Per-segment context for the channel jobs (sources are rendered inside each job).
    bool anyRolling = false;
    int64_t automationPos = 0;
    jobNumSegs_ = numSegs;
    jobFrames_ = frames;
    jobNumIn_ = nIn;
    for (int c = 0; c < nIn; ++c) jobIn_[c] = inPtrs[c];
    for (int s = 0; s < numSegs; ++s) {
        const auto& seg = segs[s];
        const bool stopping = wasRolling_ && !seg.rolling;
        jobSegs_[s] = seg;
        jobNotesOff_[s] = seg.jumped || stopping;
        if (seg.jumped) metronome_.reset();
        if (seg.rolling) {
            if (!anyRolling) automationPos = seg.timelineStart;
            anyRolling = true;
        }
        wasRolling_ = seg.rolling;
        if ((seg.rolling || seg.countIn) && g)
            metronome_.render(g->tempo, seg.timelineStart, seg.numFrames, mL + seg.blockOffset, mR + seg.blockOffset,
                              seg.countIn);
    }

    if (!offlineRendering_) collectLiveMidi(anyRolling, automationPos, outOffset, frames);
    else liveCount_ = 0, liveBlockTarget_ = -1, liveOffTarget_ = -1;

    if (auto* l = listener_.load(std::memory_order_acquire)) l->onAudioInput(inPtrs, nIn, frames, segs, numSegs);

    if (!g) {
        for (int c = 0; c < numOutputs; ++c)
            for (int i = 0; i < frames; ++i) outputs[c][outOffset + i] = (c % 2 == 0 ? mL : mR)[i];
        if (!offlineRendering_) preview_.render(outputs, numOutputs, outOffset, frames);
        return;
    }

    if (anyRolling) {
        for (auto& curve : g->automation) {
            const float v = curve.valueAt(automationPos);
            const auto& t = curve.target;
            if (t.channel < 0) continue;
            auto& params = *g->channels[static_cast<size_t>(t.channel)].params;
            switch (t.kind) {
            case AutomationTarget::Gain: params.gainDb.store(v, std::memory_order_relaxed); break;
            case AutomationTarget::Pan: params.pan.store(v, std::memory_order_relaxed); break;
            case AutomationTarget::Width: params.width.store(v, std::memory_order_relaxed); break;
            case AutomationTarget::ProcessorParam:
                if (t.processor) t.processor->setParam(t.paramIndex, v);
                break;
            case AutomationTarget::SendLevel:
                if (t.paramIndex >= 0 && t.paramIndex < ChannelParams::kMaxSends)
                    params.sendLevelDb[t.paramIndex].store(v, std::memory_order_relaxed);
                break;
            }
        }
    }

    // Channels level by level; channels inside a level run in parallel on the worker pool.
    if (g->levelStart.size() >= 2) {
        for (int lv = 0; lv + 1 < static_cast<int>(g->levelStart.size()); ++lv) runLevel(*g, lv);
    } else {
        for (int i = 0; i < static_cast<int>(g->channels.size()); ++i) runChannel(*g, i);
    }

    if (capture_ && capture_->writePos + frames <= capture_->capacity) {
        for (size_t k = 0; k < capture_->channelIds.size(); ++k)
            for (auto& ch : g->channels)
                if (ch.id == capture_->channelIds[k]) {
                    for (int c = 0; c < 2; ++c)
                        std::memcpy(capture_->data[k][static_cast<size_t>(c)].data() + capture_->writePos, ch.out.channel(c),
                                    sizeof(float) * static_cast<size_t>(frames));
                    break;
                }
        capture_->writePos += frames;
    }

    const GraphChannel& master = g->channels[static_cast<size_t>(g->master)];
    const float* outL = master.out.channel(0);
    const float* outR = master.out.channel(1);
    uint64_t fixes = 0;
    const bool previewOn = !offlineRendering_ && numOutputs >= 1;
    for (int c = 0; c < numOutputs; ++c) {
        float* dst = outputs[c] + outOffset;
        if (numOutputs == 1) {
            for (int i = 0; i < frames; ++i) dst[i] = 0.5f * (outL[i] + outR[i]) + 0.5f * (mL[i] + mR[i]);
        } else if (c < 2) {
            const float* src = c == 0 ? outL : outR;
            const float* met = c == 0 ? mL : mR;
            for (int i = 0; i < frames; ++i) dst[i] = src[i] + met[i];
        } else {
            std::memset(dst, 0, sizeof(float) * static_cast<size_t>(frames));
        }
        for (int i = 0; i < frames; ++i)
            if (!std::isfinite(dst[i])) {
                dst[i] = 0.0f;
                ++fixes;
            }
    }
    if (previewOn) preview_.render(outputs, numOutputs, outOffset, frames);
    if (!offlineRendering_ && outputProtection_.load(std::memory_order_relaxed)) {
        // soft knee from -1 dBFS: transparent below, never above 0 dBFS (no hard clipping at the device)
        constexpr float knee = 0.891f, room = 1.0f - knee;
        uint64_t overs = 0;
        for (int c = 0; c < numOutputs; ++c) {
            float* dst = outputs[c] + outOffset;
            for (int i = 0; i < frames; ++i) {
                const float a = std::fabs(dst[i]);
                if (a <= knee) continue;
                overs += a > 1.0f;
                dst[i] = std::copysign(knee + room * std::tanh((a - knee) / room), dst[i]);
            }
        }
        if (overs) outputOvers_.fetch_add(overs, std::memory_order_relaxed);
    }
    if (fixes) {
        nonFinite_.fetch_add(fixes, std::memory_order_relaxed);
        log::audioEvent(log::Level::Error, "non-finite samples removed at output", static_cast<double>(fixes));
    }
}

bool AudioEngine::pushLiveMidi(const LiveMidiMessage& m) noexcept {
    liveReceived_.fetch_add(1, std::memory_order_relaxed);
    if (liveIn_->push(m)) return true;
    liveDropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t AudioEngine::drainRecordedMidi(std::vector<RecordedMidi>& out) {
    size_t n = 0;
    RecordedMidi r;
    while (liveRec_->pop(r)) {
        out.push_back(r);
        ++n;
    }
    return n;
}

int64_t AudioEngine::steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Audio thread, once per chunk before the channels run: live messages -> NoteEvents for the target.
void AudioEngine::collectLiveMidi(bool rolling, int64_t timeline, int outOffset, int frames) noexcept {
    liveCount_ = 0;
    const int target = liveTarget_.load(std::memory_order_acquire);
    liveOffTarget_ = target != lastLiveTarget_ ? lastLiveTarget_ : -1;
    if (target != lastLiveTarget_) {
        sustain_ = false;
        std::fill(std::begin(sustained_), std::end(sustained_), false);
    }
    lastLiveTarget_ = target;
    liveBlockTarget_ = target;
    const bool record = rolling && liveRecording_.load(std::memory_order_acquire);
    // Position of a stamped message inside this callback: the previous callback period
    // [prevCbStart, cbStart) is mapped onto [0, cbFrames). Messages that arrived after this
    // callback began wait for the next one; unstamped ones go to the start of this chunk.
    const int64_t period = cbStartNs_ - prevCbStartNs_;
    auto callbackOffset = [&](const LiveMidiMessage& msg) -> int64_t {
        if (msg.timeNs <= 0 || prevCbStartNs_ <= 0 || period <= 0) return outOffset;
        if (msg.timeNs >= cbStartNs_) return INT64_MAX; // next callback
        const int64_t rel = std::max<int64_t>(0, msg.timeNs - prevCbStartNs_);
        return std::min<int64_t>(cbFrames_ - 1, static_cast<int64_t>(static_cast<double>(rel) / static_cast<double>(period) * cbFrames_));
    };
    LiveMidiMessage m;
    while (liveCount_ < kMaxLiveEvents) {
        if (hasPendingLive_) {
            m = pendingLive_;
        } else if (!liveIn_->pop(m)) {
            break;
        }
        const int64_t at = callbackOffset(m);
        if (at >= outOffset + frames) { // due in a later chunk or callback: keep it (FIFO order)
            pendingLive_ = m;
            hasPendingLive_ = true;
            break;
        }
        hasPendingLive_ = false;
        const int offset = static_cast<int>(std::max<int64_t>(0, at - outOffset));
        if (record) liveRec_->push(RecordedMidi{timeline + offset, m});
        const int type = m.status & 0xF0;
        NoteEvent e;
        e.offset = offset;
        e.channel = static_cast<uint8_t>(m.status & 0x0F);
        e.note = static_cast<int16_t>(m.data1 & 0x7F);
        if (type == 0x90 && m.data2 > 0) {
            e.type = NoteEvent::NoteOn;
            e.velocity = static_cast<float>(m.data2) / 127.0f;
            sustained_[e.note] = false;
        } else if (type == 0x80 || type == 0x90) {
            if (sustain_) { // pedal down: release later
                sustained_[e.note] = true;
                continue;
            }
            e.type = NoteEvent::NoteOff;
            e.velocity = 0.0f;
        } else if (type == 0xB0) {
            if (m.data1 == 64) { // sustain pedal
                const bool down = m.data2 >= 64;
                if (sustain_ && !down)
                    for (int n = 0; n < 128 && liveCount_ < kMaxLiveEvents; ++n)
                        if (sustained_[n]) {
                            sustained_[n] = false;
                            NoteEvent off;
                            off.type = NoteEvent::NoteOff;
                            off.offset = offset;
                            off.note = static_cast<int16_t>(n);
                            off.channel = e.channel;
                            liveEvents_[liveCount_++] = off;
                        }
                sustain_ = down;
                continue;
            }
            if (m.data1 == 120 || m.data1 == 123) {
                e.type = NoteEvent::AllNotesOff;
            } else {
                e.type = NoteEvent::Controller;
                e.controller = static_cast<int16_t>(m.data1);
                e.value = static_cast<float>(m.data2) / 127.0f;
            }
        } else if (type == 0xE0) {
            e.type = NoteEvent::PitchBend;
            e.value = static_cast<float>(((m.data2 & 0x7F) << 7 | (m.data1 & 0x7F)) - 8192) / 8192.0f;
        } else {
            continue; // aftertouch / program change: not used by RoY instruments yet
        }
        if (liveCount_ < kMaxLiveEvents) liveEvents_[liveCount_++] = e;
    }
}

void AudioEngine::renderSources(GraphChannel& ch, const Transport::Segment& seg) noexcept {
    const int64_t segStart = seg.timelineStart;
    const int64_t segEnd = seg.timelineStart + seg.numFrames;
    float* L = ch.in.channel(0) + seg.blockOffset;
    float* R = ch.in.channel(1) + seg.blockOffset;
    for (const auto& clip : ch.clips) {
        const int64_t s0 = std::max(segStart, clip.start);
        const int64_t s1 = std::min(segEnd, clip.end);
        if (s1 <= s0 || !clip.data) continue;
        const AudioData& d = *clip.data;
        const int64_t len = clip.end - clip.start;
        const int rightCh = d.numChannels > 1 ? 1 : 0;
        for (int64_t t = s0; t < s1; ++t) {
            const int64_t rel = t - clip.start;
            const int64_t src = clip.reversed ? clip.sourceStart + (len - 1 - rel) : clip.sourceStart + rel;
            float gain = clip.gain;
            if (clip.fadeIn > 0 && rel < clip.fadeIn)
                gain *= fadeShape(clip.fadeInCurve, static_cast<float>(rel) / static_cast<float>(clip.fadeIn));
            const int64_t remaining = clip.end - t;
            if (clip.fadeOut > 0 && remaining < clip.fadeOut)
                gain *= fadeShape(clip.fadeOutCurve, static_cast<float>(remaining) / static_cast<float>(clip.fadeOut));
            const size_t i = static_cast<size_t>(t - segStart);
            L[i] += d.sample(0, src) * gain;
            R[i] += d.sample(rightCh, src) * gain;
        }
    }
    if (!ch.notes.empty() && ch.instrument) {
        auto it = std::lower_bound(ch.notes.begin(), ch.notes.end(), segStart,
                                   [](const ScheduledNote& n, int64_t v) { return n.time < v; });
        for (; it != ch.notes.end() && it->time < segEnd; ++it) {
            if (ch.eventScratch.size() >= ch.eventScratch.capacity()) break;
            NoteEvent ev = it->ev;
            ev.offset = seg.blockOffset + static_cast<int>(it->time - segStart);
            ch.eventScratch.push_back(ev);
        }
    }
}

// One channel, complete: sources -> monitoring input -> pulled inputs -> instrument/inserts/fader.
// Touches only this channel's buffers/processors and the outputs of channels from earlier levels.
void AudioEngine::runChannel(RenderGraph& g, int index) noexcept {
    GraphChannel& ch = g.channels[static_cast<size_t>(index)];
    const int frames = jobFrames_;
    std::memset(ch.in.channel(0), 0, sizeof(float) * static_cast<size_t>(frames));
    std::memset(ch.in.channel(1), 0, sizeof(float) * static_cast<size_t>(frames));
    ch.eventScratch.clear();
    for (int s = 0; s < jobNumSegs_; ++s) {
        const auto& seg = jobSegs_[s];
        if (jobNotesOff_[s] && ch.instrument && ch.eventScratch.size() < ch.eventScratch.capacity()) {
            NoteEvent off;
            off.type = NoteEvent::AllNotesOff;
            off.offset = seg.blockOffset;
            ch.eventScratch.push_back(off);
        }
        if (seg.rolling) renderSources(ch, seg);
    }
    // live MIDI (read-only here: collected once per chunk on the audio thread)
    if (ch.instrument && index == liveOffTarget_ && ch.eventScratch.size() < ch.eventScratch.capacity()) {
        NoteEvent off;
        off.type = NoteEvent::AllNotesOff;
        ch.eventScratch.push_back(off);
    }
    if (ch.instrument && index == liveBlockTarget_)
        for (int i = 0; i < liveCount_ && ch.eventScratch.size() < ch.eventScratch.capacity(); ++i) ch.eventScratch.push_back(liveEvents_[i]);
    // input monitoring
    if (ch.inputLeft >= 0 && ch.monitorEnabled && ch.monitorEnabled->load(std::memory_order_relaxed) && ch.inputLeft < jobNumIn_) {
        const float* l = jobIn_[ch.inputLeft];
        const float* r = (ch.inputRight >= 0 && ch.inputRight < jobNumIn_) ? jobIn_[ch.inputRight] : l;
        float* dl = ch.in.channel(0);
        float* dr = ch.in.channel(1);
        for (int i = 0; i < frames; ++i) {
            dl[i] += l[i] * ch.inputGain;
            dr[i] += r[i] * ch.inputGain;
        }
    }
    // pull inputs (busses, sends) in fixed source order -> bit-identical to sequential mixing
    float* tL = ch.in.channel(0);
    float* tR = ch.in.channel(1);
    for (const auto& inc : ch.incoming) {
        GraphChannel& src = g.channels[static_cast<size_t>(inc.source)];
        Connection& c = src.outputs[static_cast<size_t>(inc.output)];
        auto& P = *src.params;
        float level = 1.0f;
        if (c.sendIndex >= 0) {
            const int si = c.sendIndex & (ChannelParams::kMaxSends - 1);
            if (!P.sendEnabled[si].load(std::memory_order_relaxed)) continue;
            level = dbToGain(P.sendLevelDb[si].load(std::memory_order_relaxed));
        }
        const float* sL = c.preFader ? src.pre.channel(0) : src.out.channel(0);
        const float* sR = c.preFader ? src.pre.channel(1) : src.out.channel(1);
        if (c.preFader && P.effectiveMute.load(std::memory_order_relaxed)) level = 0.0f;
        if (c.compensation && c.compensation->delay() > 0) {
            float* xL = c.scratch.channel(0);
            float* xR = c.scratch.channel(1);
            for (int i = 0; i < frames; ++i) {
                xL[i] = sL[i] * level;
                xR[i] = sR[i] * level;
            }
            c.compensation->process(xL, xR, frames);
            for (int i = 0; i < frames; ++i) {
                tL[i] += xL[i];
                tR[i] += xR[i];
            }
        } else {
            for (int i = 0; i < frames; ++i) {
                tL[i] += sL[i] * level;
                tR[i] += sR[i] * level;
            }
        }
    }
    processChannel(g, ch, frames);
}

void AudioEngine::processChannel(RenderGraph& g, GraphChannel& ch, int frames) noexcept {
    float* ptrs[2] = {ch.in.channel(0), ch.in.channel(1)};
    AudioBlock blk{ptrs, 2, frames};
    auto& P = *ch.params;

    if (ch.instrument) {
        // Events must be in time order for instruments.
        std::sort(ch.eventScratch.begin(), ch.eventScratch.end(),
                  [](const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
        ch.instrument->process(blk, nullptr, ch.eventScratch.data(), static_cast<int>(ch.eventScratch.size()));
    }
    for (auto& ins : ch.inserts) {
        if (P.insertBypass[ins.bypassIndex & 31].load(std::memory_order_relaxed)) continue;
        if (ins.sidechainChannel >= 0) {
            auto& sc = g.channels[static_cast<size_t>(ins.sidechainChannel)];
            float* scp[2] = {sc.out.channel(0), sc.out.channel(1)};
            AudioBlock scb{scp, 2, frames};
            ins.processor->process(blk, &scb, nullptr, 0);
        } else {
            ins.processor->process(blk, nullptr, nullptr, 0);
        }
    }

    float* inL = ch.in.channel(0);
    float* inR = ch.in.channel(1);
    std::memcpy(ch.pre.channel(0), inL, sizeof(float) * static_cast<size_t>(frames));
    std::memcpy(ch.pre.channel(1), inR, sizeof(float) * static_cast<size_t>(frames));

    // ---- fader section ----
    const bool mute = P.effectiveMute.load(std::memory_order_relaxed);
    const float gain = mute ? 0.0f : dbToGain(P.gainDb.load(std::memory_order_relaxed));
    float pl, pr;
    panGains(P.pan.load(std::memory_order_relaxed), pl, pr);
    const float width = std::clamp(P.width.load(std::memory_order_relaxed), 0.0f, 2.0f);
    const float phase = P.phaseInvert.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float targetL = gain * pl * phase, targetR = gain * pr * phase;
    if (!ch.gainInitialised) {
        ch.lastGainL = targetL;
        ch.lastGainR = targetR;
        ch.gainInitialised = true;
    }
    float* oL = ch.out.channel(0);
    float* oR = ch.out.channel(1);
    const float stepL = (targetL - ch.lastGainL) / static_cast<float>(frames);
    const float stepR = (targetR - ch.lastGainR) / static_cast<float>(frames);
    float gl = ch.lastGainL, gr = ch.lastGainR;
    float peakL = 0, peakR = 0;
    double sumL = 0, sumR = 0;
    for (int i = 0; i < frames; ++i) {
        float l = inL[i], r = inR[i];
        if (width != 1.0f) {
            const float m = 0.5f * (l + r);
            const float s = 0.5f * (l - r) * width;
            l = m + s;
            r = m - s;
        }
        gl += stepL;
        gr += stepR;
        l *= gl;
        r *= gr;
        oL[i] = l;
        oR[i] = r;
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
        sumL += static_cast<double>(l) * l;
        sumR += static_cast<double>(r) * r;
    }
    ch.lastGainL = targetL;
    ch.lastGainR = targetR;

    if (peakL > P.peakL.load(std::memory_order_relaxed)) P.peakL.store(peakL, std::memory_order_relaxed);
    if (peakR > P.peakR.load(std::memory_order_relaxed)) P.peakR.store(peakR, std::memory_order_relaxed);
    P.rmsL.store(static_cast<float>(std::sqrt(sumL / frames)), std::memory_order_relaxed);
    P.rmsR.store(static_cast<float>(std::sqrt(sumR / frames)), std::memory_order_relaxed);
    if (peakL > 1.0f || peakR > 1.0f) P.clipCount.fetch_add(1, std::memory_order_relaxed);
}

} // namespace roy
