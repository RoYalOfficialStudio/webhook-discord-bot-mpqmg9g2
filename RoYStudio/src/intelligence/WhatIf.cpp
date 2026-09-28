#include "intelligence/WhatIf.h"
#include "audio/OfflineRender.h"
#include "audio/ProjectRuntime.h"
#include "dsp/Analysis.h"
#include "dsp/Loudness.h"
#include "intelligence/MixIntelligence.h"
#include "project/ProjectIO.h"

#include <cmath>

namespace roy::whatif {

void WhatIfSession::setState(const std::string& state) {
    Project p;
    if (!deserializeProject(state, p, &error_)) return;
    ctx_.project = std::move(p);
    if (ctx_.changed) ctx_.changed(true);
}

bool WhatIfSession::begin(const std::string& name, const Steps& steps) {
    if (active_) discard();
    error_.clear();
    name_ = name;
    stateA_ = serializeProject(ctx_.project, false);
    // Apply all steps; on failure restore A and report.
    for (auto& [id, args] : steps) {
        const CommandInfo* c = reg_.find(id);
        if (!c) {
            error_ = "unknown command " + id;
            setState(stateA_);
            return false;
        }
        if (!c->fn(ctx_, args)) {
            error_ = id + ": " + ctx_.error;
            setState(stateA_);
            return false;
        }
    }
    stateB_ = serializeProject(ctx_.project, false);
    active_ = true;
    showingB_ = true;
    if (ctx_.changed) ctx_.changed(true);
    return true;
}

void WhatIfSession::showA() {
    if (!active_ || !showingB_) return;
    setState(stateA_);
    showingB_ = false;
}

void WhatIfSession::showB() {
    if (!active_ || showingB_) return;
    setState(stateB_);
    showingB_ = true;
}

bool WhatIfSession::commit() {
    if (!active_) return false;
    // Record exactly one undo step A -> B.
    setState(stateA_);
    ctx_.undo.begin("What-If: " + name_);
    setState(stateB_);
    ctx_.undo.end();
    active_ = false;
    showingB_ = false;
    return true;
}

void WhatIfSession::discard() {
    if (!active_) return;
    setState(stateA_);
    active_ = false;
    showingB_ = false;
}

std::optional<ABComparison> compareOffline(AudioEngine& engine, CommandRegistry& registry, CommandContext& ctx, const Steps& steps,
                                           double startBeat, double endBeat, std::string* error) {
    if (!ctx.runtime) {
        if (error) *error = "no audio runtime";
        return std::nullopt;
    }
    const double sr = engine.sampleRate();
    OfflineRenderOptions o;
    o.startSample = static_cast<int64_t>(ctx.project.tempo.beatToSample(startBeat, sr));
    o.numFrames = static_cast<int64_t>(ctx.project.tempo.beatToSample(endBeat, sr)) - o.startSample;
    auto saved = ctx.changed;
    ctx.changed = nullptr; // this comparison drives the runtime itself
    WhatIfSession s(registry, ctx);
    ABComparison cmp;
    bool ok = ctx.runtime->rebuild(ctx.project);
    if (ok) cmp.renderA = renderOffline(engine, o, error);
    if (ok && s.begin("compare", steps)) {
        ok = ctx.runtime->rebuild(ctx.project);
        if (ok) cmp.renderB = renderOffline(engine, o, error);
        s.discard();
    } else if (ok) {
        if (error) *error = s.error();
        ok = false;
    }
    ctx.runtime->rebuild(ctx.project);
    ctx.changed = saved;
    if (!ok || cmp.renderA.empty() || cmp.renderB.empty()) return std::nullopt;
    auto la = dsp::measureLoudness(cmp.renderA, sr), lb = dsp::measureLoudness(cmp.renderB, sr);
    cmp.lufsA = la.integratedLufs;
    cmp.lufsB = lb.integratedLufs;
    cmp.truePeakA = la.truePeakDb;
    cmp.truePeakB = lb.truePeakDb;
    // average third-octave band difference
    const auto ma = dsp::mixToMono(cmp.renderA), mb = dsp::mixToMono(cmp.renderB);
    const auto sa = dsp::averageSpectrumDb(ma.data(), static_cast<int64_t>(ma.size()), sr, 4096);
    const auto sb = dsp::averageSpectrumDb(mb.data(), static_cast<int64_t>(mb.size()), sr, 4096);
    for (double c : mixi::thirdOctaveCentres()) {
        const size_t b0 = static_cast<size_t>(c / std::pow(2.0, 1.0 / 6.0) * 4096 / sr);
        const size_t b1 = std::max(b0 + 1, static_cast<size_t>(c * std::pow(2.0, 1.0 / 6.0) * 4096 / sr));
        double ea = 0, eb = 0;
        for (size_t b = b0; b < b1 && b < sa.size(); ++b) {
            ea += std::pow(10.0, sa[b] / 10.0);
            eb += std::pow(10.0, sb[b] / 10.0);
        }
        cmp.bandDiffDb.push_back(10.0 * std::log10((eb + 1e-20) / (ea + 1e-20)));
    }
    return cmp;
}

} // namespace roy::whatif
