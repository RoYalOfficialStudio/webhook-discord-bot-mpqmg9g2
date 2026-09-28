#include "audio/OfflineRender.h"

#include <algorithm>

namespace roy {

std::vector<std::vector<float>> renderOffline(AudioEngine& engine, const OfflineRenderOptions& opt, std::string* error) {
    if (engine.isDeviceRunning()) {
        if (error) *error = "stop the audio device before offline rendering";
        return {};
    }
    if (opt.numFrames <= 0) {
        if (error) *error = "nothing to render";
        return {};
    }
    auto& t = engine.transport();
    const int64_t savedCountIn = t.countInSamples();
    const int64_t savedPreRoll = t.preRollSamples();
    const bool savedLoop = t.loopEnabled();
    const int64_t savedLoopStart = t.loopStart(), savedLoopEnd = t.loopEnd();
    const bool savedMetro = engine.metronome().enabled();

    t.stop();
    t.setCountInSamples(0);
    t.setPreRollSamples(0);
    t.setLoop(false, savedLoopStart, savedLoopEnd);
    engine.metronome().setEnabled(opt.includeMetronome);
    t.seek(opt.startSample);
    engine.resetProcessingState();
    t.play();
    if (opt.capture) {
        opt.capture->writePos = 0;
        engine.setCapture(opt.capture);
    }

    const int block = std::clamp(opt.blockSize, 16, engine.maxBlockSize());
    const int nOut = std::max(1, opt.numOutputs);
    std::vector<std::vector<float>> out(static_cast<size_t>(nOut), std::vector<float>(static_cast<size_t>(opt.numFrames)));
    std::vector<float*> ptrs(static_cast<size_t>(nOut));
    bool cancelled = false;
    for (int64_t done = 0; done < opt.numFrames; done += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, opt.numFrames - done));
        for (int c = 0; c < nOut; ++c) ptrs[static_cast<size_t>(c)] = out[static_cast<size_t>(c)].data() + done;
        engine.process(nullptr, 0, ptrs.data(), nOut, n);
        if (opt.progress && !opt.progress(static_cast<double>(done + n) / static_cast<double>(opt.numFrames))) {
            cancelled = true;
            break;
        }
    }
    engine.setCapture(nullptr);
    t.stop();
    t.setCountInSamples(savedCountIn);
    t.setPreRollSamples(savedPreRoll);
    t.setLoop(savedLoop, savedLoopStart, savedLoopEnd);
    engine.metronome().setEnabled(savedMetro);
    // Apply the queued transport requests so the engine is in a clean state.
    float dummyL[16] = {}, dummyR[16] = {};
    float* d[2] = {dummyL, dummyR};
    engine.process(nullptr, 0, d, 2, 1);
    engine.collectGarbage();
    if (cancelled) {
        if (error) *error = "cancelled";
        return {};
    }
    return out;
}

} // namespace roy
