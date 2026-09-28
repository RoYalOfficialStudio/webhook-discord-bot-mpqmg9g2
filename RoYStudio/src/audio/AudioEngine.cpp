#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace roy {

float AutomationCurve::valueAt(int64_t t) const noexcept {
    if (points.empty()) return 0.0f;
    if (t <= points.front().first) return points.front().second;
    if (t >= points.back().first) return points.back().second;
    auto it = std::upper_bound(points.begin(), points.end(), t,
                               [](int64_t v, const std::pair<int64_t, float>& p) { return v < p.first; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = static_cast<double>(b.first - a.first);
    const double x = span > 0 ? static_cast<double>(t - a.first) / span : 0.0;
    return static_cast<float>(a.second + (b.second - a.second) * x);
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

AudioEngine::AudioEngine() { prepare(48000.0, 512); }

AudioEngine::~AudioEngine() {
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
    return s;
}

void AudioEngine::resetStats() {
    peakCpuLoad_.store(0.0);
    overloads_.store(0);
    nonFinite_.store(0);
}

void AudioEngine::process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                          int numFrames) noexcept {
    ScopedNoDenormals noDenormals;
    inProcess_.store(true, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();

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

    if (g) {
        for (auto& ch : g->channels) {
            std::memset(ch.in.channel(0), 0, sizeof(float) * static_cast<size_t>(frames));
            std::memset(ch.in.channel(1), 0, sizeof(float) * static_cast<size_t>(frames));
            ch.eventScratch.clear();
        }
    }

    bool anyRolling = false;
    int64_t automationPos = 0;
    for (int s = 0; s < numSegs; ++s) {
        const auto& seg = segs[s];
        const bool stopping = wasRolling_ && !seg.rolling;
        if (g && (seg.jumped || stopping)) {
            for (auto& ch : g->channels)
                if (ch.instrument && ch.eventScratch.size() < ch.eventScratch.capacity()) {
                    NoteEvent off;
                    off.type = NoteEvent::AllNotesOff;
                    off.offset = seg.blockOffset;
                    ch.eventScratch.push_back(off);
                }
        }
        if (seg.jumped) metronome_.reset();
        if (seg.rolling && g) {
            if (!anyRolling) automationPos = seg.timelineStart;
            anyRolling = true;
            renderSources(*g, seg);
        }
        wasRolling_ = seg.rolling;
        if ((seg.rolling || seg.countIn) && g)
            metronome_.render(g->tempo, seg.timelineStart, seg.numFrames, mL + seg.blockOffset, mR + seg.blockOffset,
                              seg.countIn);
    }

    if (auto* l = listener_.load(std::memory_order_acquire)) l->onAudioInput(inPtrs, nIn, frames, segs, numSegs);

    if (!g) {
        for (int c = 0; c < numOutputs; ++c)
            for (int i = 0; i < frames; ++i) outputs[c][outOffset + i] = (c % 2 == 0 ? mL : mR)[i];
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
            }
        }
    }

    // Input monitoring for armed/monitored channels.
    for (auto& ch : g->channels) {
        if (ch.inputLeft < 0 || !ch.monitorEnabled || !ch.monitorEnabled->load(std::memory_order_relaxed)) continue;
        if (ch.inputLeft >= nIn) continue;
        const float* l = inPtrs[ch.inputLeft];
        const float* r = (ch.inputRight >= 0 && ch.inputRight < nIn) ? inPtrs[ch.inputRight] : l;
        float* dl = ch.in.channel(0);
        float* dr = ch.in.channel(1);
        for (int i = 0; i < frames; ++i) {
            dl[i] += l[i] * ch.inputGain;
            dr[i] += r[i] * ch.inputGain;
        }
    }

    for (auto& ch : g->channels) processChannel(*g, ch, frames);

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
    if (fixes) {
        nonFinite_.fetch_add(fixes, std::memory_order_relaxed);
        log::audioEvent(log::Level::Error, "non-finite samples removed at output", static_cast<double>(fixes));
    }
}

void AudioEngine::renderSources(RenderGraph& g, const Transport::Segment& seg) noexcept {
    const int64_t segStart = seg.timelineStart;
    const int64_t segEnd = seg.timelineStart + seg.numFrames;
    for (auto& ch : g.channels) {
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

    // ---- outputs (sends + main out) ----
    for (auto& c : ch.outputs) {
        if (c.target < 0) continue;
        float level = 1.0f;
        if (c.sendIndex >= 0) {
            const int si = c.sendIndex & (ChannelParams::kMaxSends - 1);
            if (!P.sendEnabled[si].load(std::memory_order_relaxed)) continue;
            level = dbToGain(P.sendLevelDb[si].load(std::memory_order_relaxed));
        }
        const float* sL = c.preFader ? ch.pre.channel(0) : oL;
        const float* sR = c.preFader ? ch.pre.channel(1) : oR;
        if (c.preFader && mute) level = 0.0f;
        auto& target = g.channels[static_cast<size_t>(c.target)];
        float* tL = target.in.channel(0);
        float* tR = target.in.channel(1);
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
}

} // namespace roy
