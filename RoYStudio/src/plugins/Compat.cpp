#include "plugins/Compat.h"
#include "core/AudioBuffer.h"
#include "core/Math.h"
#include "core/Process.h"
#include "plugins/Sandbox.h"

#include <chrono>
#include <cmath>
#include <format>
#include <thread>

namespace roy::plugins {

namespace {
// Renders `seconds` of audio: effects get a -12 dBFS 440 Hz tone, instruments a C4 note (released at half time).
bool runAudio(SandboxedPluginProcessor& p, const CompatOptions& o, double& rmsDb, bool& finite) {
    const int block = o.blockSize;
    AudioBuffer buf(2, block);
    const int64_t total = static_cast<int64_t>(o.seconds * o.sampleRate);
    double e = 0;
    int64_t n = 0;
    finite = true;
    for (int64_t pos = 0; pos < total; pos += block) {
        const int frames = static_cast<int>(std::min<int64_t>(block, total - pos));
        buf.clear();
        if (!p.isInstrument())
            for (int i = 0; i < frames; ++i) {
                const float v = 0.25f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(pos + i) / o.sampleRate));
                buf.channel(0)[i] = v;
                buf.channel(1)[i] = v;
            }
        NoteEvent ev[1];
        int nev = 0;
        if (p.isInstrument() && pos == 0) {
            ev[0].type = NoteEvent::NoteOn;
            ev[0].note = 60;
            ev[0].velocity = 0.8f;
            nev = 1;
        } else if (p.isInstrument() && pos <= total / 2 && pos + block > total / 2) {
            ev[0].type = NoteEvent::NoteOff;
            ev[0].note = 60;
            nev = 1;
        }
        AudioBlock blk{buf.channels(), 2, frames};
        p.process(blk, nullptr, ev, nev);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < frames; ++i) {
                const float v = buf.channel(c)[i];
                if (!std::isfinite(v)) finite = false;
                e += static_cast<double>(v) * v;
            }
        n += 2 * frames;
    }
    rmsDb = n ? 10.0 * std::log10(e / static_cast<double>(n) + 1e-30) : -200.0;
    return p.alive();
}
} // namespace

CompatResult checkPlugin(const PluginRecord& rec, const CompatOptions& o) {
    CompatResult r;
    r.typeId = rec.typeId;
    r.name = rec.name;
    r.vendor = rec.vendor;
    r.format = rec.format;
    r.category = rec.category;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    auto p = SandboxedPluginProcessor::create(rec.typeId, &err);
    r.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!p) {
        r.notes.push_back("load failed: " + err);
        return r;
    }
    r.loaded = true;
    r.params = p->numParams();
    p->prepare(o.sampleRate, o.blockSize);
    r.latency = p->latencySamples();
    bool finite = true;
    r.audioOk = runAudio(*p, o, r.outRmsDb, finite) && finite && p->invalidSamples() == 0;
    if (!finite) r.notes.push_back("non-finite samples reached the host output");
    if (p->invalidSamples())
        r.notes.push_back(std::format("plugin produced {} NaN/Inf samples - RoY replaced them with silence (plugin bug)", p->invalidSamples()));
    if (!p->alive()) r.notes.push_back("host died during processing: " + p->problem());
    r.audible = r.outRmsDb > -80.0;
    if (r.audioOk && !r.audible) r.notes.push_back("silent output (may be expected, e.g. MIDI/utility plugins)");
    // parameters: move every automatable parameter a little, then state round trip into a new instance
    for (int i = 0; i < p->numParams(); ++i) {
        const auto& pi = p->paramInfo(i);
        if (pi.steps == 0 && pi.maxValue > pi.minValue) p->setParam(i, pi.minValue + (pi.maxValue - pi.minValue) * 0.37f);
    }
    double dummy = 0;
    runAudio(*p, CompatOptions{false, 0.05, o.sampleRate, o.blockSize}, dummy, finite); // let the plugin see the new values
    const json saved = p->saveState();
    auto q = SandboxedPluginProcessor::create(rec.typeId, &err);
    if (!q) {
        r.notes.push_back("second instance failed: " + err);
    } else {
        q->prepare(o.sampleRate, o.blockSize);
        q->loadState(saved);
        if (!q->loadWarning().empty()) r.notes.push_back(q->loadWarning());
        int diffs = 0;
        std::string first;
        for (int i = 0; i < p->numParams() && i < q->numParams(); ++i)
            if (std::fabs(p->getParam(i) - q->getParam(i)) > 1e-4f * std::max(1.0f, std::fabs(p->paramInfo(i).maxValue - p->paramInfo(i).minValue))) {
                if (!diffs) first = std::format("{} ({} vs {})", p->paramInfo(i).name, p->getParam(i), q->getParam(i));
                ++diffs;
            }
        r.stateOk = diffs == 0 && q->alive() && q->numParams() == p->numParams();
        if (diffs) r.notes.push_back(std::format("{} parameter(s) differ after state restore, e.g. {}", diffs, first));
        double rq = 0;
        if (!runAudio(*q, CompatOptions{false, 0.1, o.sampleRate, o.blockSize}, rq, finite) || !finite || q->invalidSamples())
            r.notes.push_back("restored instance: invalid output too");
    }
    if (o.editor) {
        r.editorChecked = true;
        const json st = p->editorState();
        r.hasEditor = st.value("available", true);
        if (p->openEditor(false, &err)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const bool open = p->editorState().value("open", false);
            p->closeEditor();
            r.editorOk = open && !p->editorState().value("open", true) && p->alive();
            if (!open) r.notes.push_back("editor reported not open");
        } else {
            r.editorOk = false;
            if (err.find("no editor") != std::string::npos || err.find("not support") != std::string::npos ||
                err.find("no embeddable editor") != std::string::npos)
                r.hasEditor = false;
            r.notes.push_back("editor: " + err + (p->alive() ? "" : " [" + p->problem() + "]"));
        }
    }
    // unload: the sandbox process must end
    const int pid = p->hostPid(), pid2 = q ? q->hostPid() : 0;
    p.reset();
    q.reset();
    bool gone = false;
    for (int i = 0; i < 300 && !gone; ++i) {
        gone = !isProcessAlive(pid) && (pid2 == 0 || !isProcessAlive(pid2));
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    r.unloaded = gone;
    if (!gone) r.notes.push_back("host process still running after unload");
    return r;
}

std::vector<CompatResult> checkAll(const PluginDatabase& db, const CompatOptions& o) {
    std::vector<CompatResult> out;
    for (auto* rec : db.installed()) out.push_back(checkPlugin(*rec, o));
    return out;
}

std::string compatReportMarkdown(const std::vector<CompatResult>& res, const CompatOptions& o) {
    int pass = 0;
    for (auto& r : res) pass += r.pass();
    std::string md = std::format("# Plugin compatibility report\n\n{} of {} plugins PASS | {:.1f} s audio each @ {:.0f} Hz, block {} | editors {}\n\n",
                                 pass, res.size(), o.seconds, o.sampleRate, o.blockSize, o.editor ? "checked" : "not checked");
    md += "| Plugin | Vendor | Format | Kind | Params | Load | Audio | Out RMS | State | Editor | Unload | Result | Notes |\n";
    md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    auto yn = [](bool b) { return b ? "PASS" : "FAIL"; };
    for (auto& r : res) {
        std::string notes;
        for (auto& n : r.notes) notes += (notes.empty() ? "" : "; ") + n;
        const std::string editor = !r.editorChecked ? "-" : !r.hasEditor ? "none" : yn(r.editorOk);
        md += std::format("| {} | {} | {} | {} | {} | {:.0f} ms | {} | {:.0f} dB | {} | {} | {} | **{}** | {} |\n", r.name, r.vendor, r.format,
                          r.category, r.params, r.loadMs, r.loaded ? yn(r.audioOk) : "-", r.outRmsDb, r.loaded ? yn(r.stateOk) : "-", editor,
                          r.loaded ? yn(r.unloaded) : "-", r.pass() ? "PASS" : "FAIL", notes);
    }
    return md;
}

} // namespace roy::plugins
