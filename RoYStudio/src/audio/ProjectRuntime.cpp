#include "audio/ProjectRuntime.h"
#include "core/Log.h"
#include "core/Math.h"
#include "dsp/TimeStretch.h"
#include "instruments/Drums.h"
#include "io/AudioFile.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <set>

namespace roy {

namespace fs = std::filesystem;

ProjectRuntime::ProjectRuntime(AudioEngine& engine) : engine_(engine) {
    // Default clip time-stretch / pitch engine (WSOLA + band-limited resampling).
    derive_ = [](const AudioData& src, double stretch, double semitones) {
        auto d = std::make_shared<AudioData>();
        d->sampleRate = src.sampleRate;
        d->numChannels = src.numChannels;
        d->channels = dsp::stretchAndShift(src.channels, stretch, semitones, src.sampleRate);
        d->numFrames = d->channels.empty() ? 0 : static_cast<int64_t>(d->channels[0].size());
        return d;
    };
}
ProjectRuntime::~ProjectRuntime() = default;

fs::path ProjectRuntime::resolveAssetPath(const AudioAsset& a) const {
    fs::path p(a.path);
    if (p.is_absolute() || projectDir_.empty()) return p;
    return projectDir_ / p;
}

std::shared_ptr<const AudioData> ProjectRuntime::asset(const Project& project, const std::string& assetId) {
    auto it = assets_.find(assetId);
    if (it != assets_.end() && it->second && std::fabs(it->second->sampleRate - engine_.sampleRate()) < 0.5)
        return it->second;
    const AudioAsset* a = project.findAsset(assetId);
    if (!a) return nullptr;
    std::string err;
    auto data = loadAudioAt(resolveAssetPath(*a), engine_.sampleRate(), &err);
    if (!data) {
        warnings_.push_back(std::format("asset '{}' ({}) could not be loaded: {}", a->originalName, a->path, err));
        return nullptr;
    }
    data->assetId = assetId;
    assets_[assetId] = data;
    return data;
}

void ProjectRuntime::addLoadedAsset(const std::string& assetId, std::shared_ptr<const AudioData> data) {
    assets_[assetId] = std::move(data);
}

void ProjectRuntime::clearAssetCache() {
    assets_.clear();
    derivedAssets_.clear();
}

std::shared_ptr<const AudioData> ProjectRuntime::derived(const Project& p, const AudioClip& c) {
    auto src = asset(p, c.assetId);
    if (!src) return nullptr;
    if (std::fabs(c.stretch - 1.0) < 1e-6 && std::fabs(c.pitchSemitones) < 1e-6) return src;
    const std::string key = std::format("{}|{:.6f}|{:.4f}|{}", c.assetId, c.stretch, c.pitchSemitones, engine_.sampleRate());
    auto it = derivedAssets_.find(key);
    if (it != derivedAssets_.end()) return it->second;
    if (!derive_) {
        warnings_.push_back("clip " + c.name + ": stretch/pitch requested but no time-stretch engine installed; playing unstretched");
        return src;
    }
    auto d = derive_(*src, c.stretch, c.pitchSemitones);
    if (!d) return src;
    d->assetId = key;
    derivedAssets_[key] = d;
    return d;
}

namespace {
// Non-parameter part of a slot state (sample maps, plugin chunks, ...).
std::string opaqueStateOf(const PluginSlot& slot) {
    if (!slot.state.is_object()) return {};
    json j = slot.state;
    j.erase("params");
    return j.dump();
}
} // namespace

std::shared_ptr<Processor> ProjectRuntime::ensureProcessor(const Project& project, const PluginSlot& slot, double sr, int maxBlock) {
    const std::string opaque = opaqueStateOf(slot);
    auto it = processors_.find(slot.id);
    if (it != processors_.end() && it->second.typeId == slot.typeId && it->second.proc && it->second.opaqueState == opaque) {
        auto& e = it->second;
        if (e.sr != sr || e.block != maxBlock) {
            // Sample rate / block size change only happens with the device stopped.
            e.proc->prepare(sr, maxBlock);
            e.sr = sr;
            e.block = maxBlock;
        }
        return e.proc;
    }
    // New slot, changed type or changed opaque state: a fresh instance (the old
    // one stays alive in the current graph until that graph is retired).
    auto proc = std::shared_ptr<Processor>(ProcessorFactory::instance().create(slot.typeId));
    if (!proc) {
        warnings_.push_back(std::format("processor type '{}' ({}) is not available - slot bypassed", slot.typeId, slot.name));
        return nullptr;
    }
    proc->loadState(slot.state);
    for (auto& id : proc->requiredAssets()) {
        auto data = asset(project, id);
        if (!data) warnings_.push_back(std::format("{}: sample asset {} missing", slot.name, id));
        proc->setAsset(id, data);
    }
    proc->prepare(sr, maxBlock);
    processors_[slot.id] = ProcEntry{slot.typeId, proc, sr, maxBlock, opaque};
    return proc;
}

std::shared_ptr<Processor> ProjectRuntime::processorForSlot(const std::string& slotId) const {
    auto it = processors_.find(slotId);
    return it == processors_.end() ? nullptr : it->second.proc;
}

std::shared_ptr<ChannelParams> ProjectRuntime::channelParams(const std::string& channelId) const {
    auto it = params_.find(channelId);
    return it == params_.end() ? nullptr : it->second;
}

std::shared_ptr<std::atomic<bool>> ProjectRuntime::monitorFlag(const std::string& trackId) const {
    auto it = monitors_.find(trackId);
    return it == monitors_.end() ? nullptr : it->second;
}

void ProjectRuntime::captureProcessorStates(Project& project) const {
    auto capture = [&](PluginSlot& s) {
        if (auto p = processorForSlot(s.id)) s.state = p->saveState();
    };
    for (auto& ch : project.channels)
        for (auto& s : ch.inserts) capture(s);
    for (auto& t : project.tracks)
        if (t.instrument) capture(*t.instrument);
}

bool ProjectRuntime::rebuild(const Project& project) {
    warnings_.clear();
    const double sr = engine_.sampleRate();
    const int maxBlock = engine_.maxBlockSize();
    auto toSample = [&](double beat) { return static_cast<int64_t>(std::llround(project.tempo.beatToSample(beat, sr))); };

    // ---- channel ordering (topological, master last) -------------------------
    const MixerChannel* master = project.master();
    if (!master) {
        warnings_.push_back("project has no master channel");
        return false;
    }
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < project.channels.size(); ++i) idx[project.channels[i].id] = i;
    auto outputOf = [&](const MixerChannel& c) -> std::string {
        if (c.kind == ChannelKind::Master) return {};
        if (!c.outputChannelId.empty() && idx.count(c.outputChannelId) && c.outputChannelId != c.id) return c.outputChannelId;
        return master->id;
    };
    // edges: from -> to (from must be processed before to)
    std::map<std::string, std::set<std::string>> edges;
    std::map<std::string, int> indeg;
    for (auto& c : project.channels) indeg[c.id] = 0;
    auto addEdge = [&](const std::string& from, const std::string& to) {
        if (from == to || !idx.count(from) || !idx.count(to)) return false;
        if (edges[from].insert(to).second) indeg[to]++;
        return true;
    };
    // Checks whether adding from->to would create a cycle (path to -> from exists).
    std::function<bool(const std::string&, const std::string&, std::set<std::string>&)> reaches =
        [&](const std::string& a, const std::string& b, std::set<std::string>& seen) -> bool {
        if (a == b) return true;
        if (!seen.insert(a).second) return false;
        for (auto& n : edges[a])
            if (reaches(n, b, seen)) return true;
        return false;
    };
    std::set<std::pair<std::string, std::string>> rejected;
    auto safeEdge = [&](const std::string& from, const std::string& to, const std::string& what) {
        std::set<std::string> seen;
        if (reaches(to, from, seen)) {
            warnings_.push_back(std::format("routing {} would create a feedback loop - ignored", what));
            rejected.insert({from, to});
            return false;
        }
        return addEdge(from, to);
    };
    for (auto& c : project.channels) {
        const auto out = outputOf(c);
        if (!out.empty() && !safeEdge(c.id, out, "output of " + c.name)) safeEdge(c.id, master->id, "fallback of " + c.name);
    }
    for (auto& c : project.channels)
        for (auto& s : c.sends) safeEdge(c.id, s.targetChannelId, "send from " + c.name);
    for (auto& c : project.channels)
        for (auto& ins : c.inserts)
            if (!ins.sidechainChannelId.empty()) safeEdge(ins.sidechainChannelId, c.id, "sidechain into " + c.name);

    std::vector<std::string> order;
    {
        std::vector<std::string> ready;
        for (auto& c : project.channels)
            if (indeg[c.id] == 0 && c.id != master->id) ready.push_back(c.id);
        auto deg = indeg;
        while (!ready.empty()) {
            const std::string n = ready.front();
            ready.erase(ready.begin());
            order.push_back(n);
            for (auto& m : edges[n])
                if (--deg[m] == 0 && m != master->id) ready.push_back(m);
        }
        order.push_back(master->id);
        if (order.size() != project.channels.size()) {
            warnings_.push_back("routing graph is not acyclic");
            return false;
        }
    }
    std::map<std::string, int> pos;
    for (size_t i = 0; i < order.size(); ++i) pos[order[i]] = static_cast<int>(i);

    // ---- graph ---------------------------------------------------------------
    auto graph = std::make_unique<RenderGraph>();
    graph->sampleRate = sr;
    graph->maxBlock = maxBlock;
    graph->tempo = project.tempo;
    graph->version = ++graphVersion_;
    graph->channels.resize(order.size());
    graph->master = static_cast<int>(order.size() - 1);

    std::map<std::string, const Track*> trackByChannel;
    for (auto& t : project.tracks) trackByChannel[t.channelId] = &t;

    std::set<std::string> liveSlots;
    for (size_t gi = 0; gi < order.size(); ++gi) {
        const MixerChannel& mc = project.channels[idx[order[gi]]];
        GraphChannel& gc = graph->channels[gi];
        gc.id = mc.id;
        gc.kind = static_cast<int>(mc.kind);
        auto& pp = params_[mc.id];
        if (!pp) pp = std::make_shared<ChannelParams>();
        gc.params = pp;
        gc.in.setSize(2, maxBlock);
        gc.out.setSize(2, maxBlock);
        gc.pre.setSize(2, maxBlock);
        gc.eventScratch.reserve(4096);

        int latency = 0;
        for (size_t s = 0; s < mc.inserts.size(); ++s) {
            const auto& slot = mc.inserts[s];
            liveSlots.insert(slot.id);
            auto proc = ensureProcessor(project, slot, sr, maxBlock);
            if (!proc) continue;
            InsertRef ref;
            ref.processor = proc;
            ref.bypassIndex = static_cast<int>(s & 31);
            if (!slot.sidechainChannelId.empty() && pos.count(slot.sidechainChannelId) &&
                !rejected.count({slot.sidechainChannelId, mc.id}) && pos[slot.sidechainChannelId] < static_cast<int>(gi))
                ref.sidechainChannel = pos[slot.sidechainChannelId];
            latency += std::max(0, proc->latencySamples());
            gc.inserts.push_back(std::move(ref));
        }

        if (auto tIt = trackByChannel.find(mc.id); tIt != trackByChannel.end()) {
            const Track& t = *tIt->second;
            auto& mon = monitors_[t.id];
            if (!mon) mon = std::make_shared<std::atomic<bool>>(false);
            mon->store(t.monitor);
            gc.monitorEnabled = mon;
            gc.inputLeft = t.inputLeft;
            gc.inputRight = t.inputRight;
            gc.inputGain = dbToGain(t.inputGainDb);
            if (t.instrument) {
                liveSlots.insert(t.instrument->id);
                gc.instrument = ensureProcessor(project, *t.instrument, sr, maxBlock);
                if (gc.instrument) latency += std::max(0, gc.instrument->latencySamples());
                // Beat Lab sample pads: rows with a sample asset play that sample.
                if (auto* drums = dynamic_cast<RoyDrums*>(gc.instrument.get())) {
                    for (auto& pc : t.patternClips)
                        if (const Pattern* pat = project.findPattern(pc.patternId))
                            for (auto& row : pat->rows)
                                drums->setSample(row.note, row.sampleAssetId.empty() ? nullptr : asset(project, row.sampleAssetId));
                }
            }
            // audio clips
            auto addClip = [&](const AudioClip& c, std::shared_ptr<const AudioData> data) {
                if (c.muted || !data || c.lengthBeats <= 0) return;
                ClipPlayback cp;
                cp.start = toSample(c.startBeat);
                cp.end = toSample(c.endBeat());
                cp.sourceStart = static_cast<int64_t>(std::llround(c.sourceOffsetSec * c.stretch * sr));
                cp.data = std::move(data);
                cp.gain = dbToGain(c.gainDb);
                cp.fadeIn = std::max<int64_t>(0, toSample(c.startBeat + c.fadeInBeats) - cp.start);
                cp.fadeOut = std::max<int64_t>(0, cp.end - toSample(c.endBeat() - c.fadeOutBeats));
                cp.fadeInCurve = static_cast<int>(c.fadeInCurve);
                cp.fadeOutCurve = static_cast<int>(c.fadeOutCurve);
                cp.reversed = c.reversed;
                gc.clips.push_back(std::move(cp));
            };
            for (auto& c : t.audioClips) addClip(c, derived(project, c));
            // comped takes
            for (auto& seg : t.comp) {
                auto tk = std::find_if(t.takes.begin(), t.takes.end(), [&](const Take& x) { return x.id == seg.takeId; });
                if (tk == t.takes.end()) continue;
                AudioClip c;
                c.assetId = tk->assetId;
                c.name = tk->name;
                c.startBeat = seg.startBeat;
                c.lengthBeats = seg.endBeat - seg.startBeat;
                c.sourceOffsetSec = project.tempo.beatToSeconds(seg.startBeat) - project.tempo.beatToSeconds(tk->startBeat);
                c.fadeInBeats = c.fadeOutBeats = std::min(0.02, c.lengthBeats / 4); // tiny crossfades at comp edges
                if (c.sourceOffsetSec < 0) continue;
                addClip(c, asset(project, c.assetId));
            }
            std::sort(gc.clips.begin(), gc.clips.end(), [](auto& a, auto& b) { return a.start < b.start; });

            // midi notes
            auto schedule = [&](double onBeat, double offBeat, int note, float vel, int chn, float pan, float detune, bool slide) {
                ScheduledNote on;
                on.time = toSample(onBeat);
                on.ev.type = NoteEvent::NoteOn;
                on.ev.note = static_cast<int16_t>(note);
                on.ev.velocity = vel;
                on.ev.channel = static_cast<uint8_t>(chn);
                on.ev.pan = pan;
                on.ev.detune = detune;
                on.ev.slide = slide;
                ScheduledNote off = on;
                off.time = std::max(on.time + 1, toSample(offBeat));
                off.ev.type = NoteEvent::NoteOff;
                off.ev.velocity = 0.0f;
                gc.notes.push_back(on);
                gc.notes.push_back(off);
            };
            for (auto& clip : t.midiClips) {
                if (clip.muted) continue;
                const double loop = clip.loopLengthBeats > 0 ? clip.loopLengthBeats : clip.lengthBeats + 1e9;
                for (int k = 0; k * loop < clip.lengthBeats && k < 10000; ++k) {
                    for (auto& n : clip.notes) {
                        if (n.muted) continue;
                        if (clip.loopLengthBeats > 0 && n.startBeat >= loop) continue;
                        const double on = n.startBeat + k * loop;
                        if (on >= clip.lengthBeats || on < 0) continue;
                        const double off = std::min(on + n.lengthBeats, clip.lengthBeats);
                        schedule(clip.startBeat + on, clip.startBeat + off, n.pitch,
                                 std::clamp(n.velocity, 1, 127) / 127.0f, n.channel, 0.0f, 0.0f, n.slide);
                    }
                }
            }
            for (auto& pc : t.patternClips) {
                if (pc.muted) continue;
                const Pattern* pat = project.findPattern(pc.patternId);
                if (!pat) {
                    warnings_.push_back("pattern clip references missing pattern " + pc.patternId);
                    continue;
                }
                uint64_t seed = 1469598103934665603ull;
                for (char ch : pc.id) seed = (seed ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
                for (auto& pn : expandPatternClip(*pat, pc, seed))
                    schedule(pn.beat, pn.beat + pn.lengthBeats, pn.note, pn.velocity, 9, pn.pan, pn.pitch, false);
            }
            // Stable order: time, then note-offs before note-ons at the same time.
            std::stable_sort(gc.notes.begin(), gc.notes.end(), [](const ScheduledNote& a, const ScheduledNote& b) {
                if (a.time != b.time) return a.time < b.time;
                return a.ev.type == NoteEvent::NoteOff && b.ev.type != NoteEvent::NoteOff;
            });
        }
        gc.chainLatency = latency;
    }
    // Drop processors whose slots no longer exist (graph keeps them alive until retired).
    std::erase_if(processors_, [&](auto& kv) { return !liveSlots.count(kv.first); });

    // ---- connections + plugin delay compensation -----------------------------
    // inputLatency(target) = max over sources of (inputLatency(src) + chainLatency(src)).
    for (size_t gi = 0; gi < order.size(); ++gi) {
        const MixerChannel& mc = project.channels[idx[order[gi]]];
        GraphChannel& gc = graph->channels[gi];
        if (mc.kind == ChannelKind::Master) continue;
        auto target = outputOf(mc);
        if (rejected.count({mc.id, target})) target = master->id;
        Connection main;
        main.target = pos[target];
        gc.outputs.push_back(std::move(main));
        for (size_t s = 0; s < mc.sends.size() && s < ChannelParams::kMaxSends; ++s) {
            const auto& send = mc.sends[s];
            if (!pos.count(send.targetChannelId) || rejected.count({mc.id, send.targetChannelId})) continue;
            if (pos[send.targetChannelId] <= static_cast<int>(gi)) continue;
            Connection c;
            c.target = pos[send.targetChannelId];
            c.sendIndex = static_cast<int>(s);
            c.preFader = send.preFader;
            gc.outputs.push_back(std::move(c));
        }
    }
    std::vector<int> inLat(order.size(), 0);
    for (size_t gi = 0; gi < order.size(); ++gi) {
        auto& gc = graph->channels[gi];
        gc.inputLatency = inLat[gi];
        const int arrival = inLat[gi] + gc.chainLatency;
        for (auto& c : gc.outputs) inLat[static_cast<size_t>(c.target)] = std::max(inLat[static_cast<size_t>(c.target)], arrival);
    }
    for (size_t gi = 0; gi < order.size(); ++gi) {
        auto& gc = graph->channels[gi];
        const int arrival = gc.inputLatency + gc.chainLatency;
        for (auto& c : gc.outputs) {
            const int comp = inLat[static_cast<size_t>(c.target)] - arrival;
            if (comp > 0) {
                c.compensation = std::make_unique<DelayLine>();
                c.compensation->setMaxDelay(comp);
                c.compensation->setDelay(comp);
                c.scratch.setSize(2, maxBlock);
            }
        }
    }
    auto& mg = graph->channels[static_cast<size_t>(graph->master)];
    graph->totalLatency = mg.inputLatency + mg.chainLatency;
    lastLatency_ = graph->totalLatency;

    // ---- automation ----------------------------------------------------------
    for (auto& lane : project.automation) {
        if (!lane.enabled || lane.points.empty() || !pos.count(lane.channelId)) continue;
        AutomationCurve curve;
        curve.target.channel = pos[lane.channelId];
        if (lane.slotId.empty()) {
            if (lane.paramId == "gain") curve.target.kind = AutomationTarget::Gain;
            else if (lane.paramId == "pan") curve.target.kind = AutomationTarget::Pan;
            else if (lane.paramId == "width") curve.target.kind = AutomationTarget::Width;
            else {
                warnings_.push_back("unknown channel automation parameter " + lane.paramId);
                continue;
            }
        } else {
            auto proc = processorForSlot(lane.slotId);
            if (!proc) continue;
            curve.target.kind = AutomationTarget::ProcessorParam;
            curve.target.processor = proc.get();
            curve.target.paramIndex = proc->findParam(lane.paramId);
            if (curve.target.paramIndex < 0) {
                warnings_.push_back("unknown automation parameter " + lane.paramId);
                continue;
            }
        }
        auto pts = lane.points;
        std::sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
        for (auto& p : pts) curve.points.emplace_back(toSample(p.beat), p.value);
        graph->automation.push_back(std::move(curve));
    }

    for (auto& w : warnings_) log::warn("runtime", "{}", w);
    syncParams(project);
    engine_.setGraph(std::move(graph));
    return true;
}

void ProjectRuntime::syncParams(const Project& project) {
    // Solo-in-place: a track is audible if soloed, or routed into a soloed bus.
    bool anySolo = false;
    for (auto& c : project.channels) anySolo |= c.solo;
    std::map<std::string, const MixerChannel*> byId;
    for (auto& c : project.channels) byId[c.id] = &c;
    auto soloedDownstream = [&](const MixerChannel& c) {
        const MixerChannel* cur = &c;
        for (int guard = 0; cur && guard < 64; ++guard) {
            if (cur->solo) return true;
            if (cur->kind == ChannelKind::Master || cur->outputChannelId.empty()) break;
            auto it = byId.find(cur->outputChannelId);
            cur = it == byId.end() ? nullptr : it->second;
        }
        return false;
    };
    for (auto& c : project.channels) {
        auto& pp = params_[c.id];
        if (!pp) pp = std::make_shared<ChannelParams>();
        pp->gainDb.store(c.gainDb);
        pp->pan.store(c.pan);
        pp->width.store(c.width);
        pp->phaseInvert.store(c.phaseInvert);
        bool muted = c.mute;
        if (anySolo && c.kind == ChannelKind::Track && !soloedDownstream(c)) muted = true;
        pp->effectiveMute.store(muted);
        for (size_t s = 0; s < c.sends.size() && s < ChannelParams::kMaxSends; ++s) {
            pp->sendLevelDb[s].store(c.sends[s].levelDb);
            pp->sendEnabled[s].store(c.sends[s].enabled);
        }
        for (size_t s = 0; s < c.inserts.size() && s < 32; ++s) pp->insertBypass[s].store(c.inserts[s].bypass);
    }
    for (auto& t : project.tracks)
        if (auto m = monitorFlag(t.id)) m->store(t.monitor);

    // The model is the source of truth for plugin parameters (undo/redo, load).
    auto applyParams = [&](const PluginSlot& slot) {
        auto proc = processorForSlot(slot.id);
        if (!proc || !slot.state.is_object()) return;
        auto it = slot.state.find("params");
        if (it == slot.state.end() || !it->is_object()) return;
        for (auto& [k, v] : it->items())
            if (v.is_number()) proc->setParam(k, v.get<float>());
    };
    for (auto& c : project.channels)
        for (auto& s : c.inserts) applyParams(s);
    for (auto& t : project.tracks)
        if (t.instrument) applyParams(*t.instrument);
}

} // namespace roy
