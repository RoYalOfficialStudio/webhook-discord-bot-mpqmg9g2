// Registration of all built-in RoY processors (effects and instruments).
#include "audio/Processor.h"
#include "effects/Dynamics.h"
#include "effects/Effects.h"
#include "effects/Special.h"
#include "instruments/Bass808.h"
#include "instruments/Drums.h"
#include "instruments/Sampler.h"
#include "instruments/Synth.h"

namespace roy {

void registerBuiltinProcessors() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& f = ProcessorFactory::instance();
    f.add("roy.synth", "RoY Synth", true, [] { return std::make_unique<RoySynth>(); });
    f.add("roy.drums", "RoY Drums", true, [] { return std::make_unique<RoyDrums>(); });
    f.add("roy.808", "RoY 808", true, [] { return std::make_unique<Bass808>(); });
    f.add("roy.sampler", "RoY Sampler", true, [] { return std::make_unique<RoySampler>(); });
    // RoY effects
    f.add("roy.eq", "RoY EQ", false, [] { return std::make_unique<RoyEq>(); });
    f.add("roy.compressor", "RoY Compressor", false, [] { return std::make_unique<RoyCompressor>(); });
    f.add("roy.limiter", "RoY Limiter", false, [] { return std::make_unique<RoyLimiter>(); });
    f.add("roy.gate", "RoY Gate", false, [] { return std::make_unique<RoyGate>(); });
    f.add("roy.deesser", "RoY De-Esser", false, [] { return std::make_unique<RoyDeEsser>(); });
    f.add("roy.reverb", "RoY Reverb", false, [] { return std::make_unique<RoyReverb>(); });
    f.add("roy.delay", "RoY Delay", false, [] { return std::make_unique<RoyDelay>(); });
    f.add("roy.saturation", "RoY Saturation", false, [] { return std::make_unique<RoySaturation>(false); });
    f.add("roy.distortion", "RoY Distortion", false, [] { return std::make_unique<RoySaturation>(true); });
    f.add("roy.chorus", "RoY Chorus", false, [] { return std::make_unique<RoyModDelay>(false); });
    f.add("roy.flanger", "RoY Flanger", false, [] { return std::make_unique<RoyModDelay>(true); });
    f.add("roy.phaser", "RoY Phaser", false, [] { return std::make_unique<RoyPhaser>(); });
    f.add("roy.stereo", "RoY Stereo", false, [] { return std::make_unique<RoyStereo>(); });
    f.add("roy.transient", "RoY Transient", false, [] { return std::make_unique<RoyTransient>(); });
    f.add("roy.clipper", "RoY Clipper", false, [] { return std::make_unique<RoyClipper>(); });
    f.add("roy.pitch", "RoY Pitch", false, [] { return std::make_unique<RoyPitch>(); });
    f.add("roy.vocaltune", "RoY VocalTune", false, [] { return std::make_unique<RoyVocalTune>(); });
    f.add("roy.noisecleaner", "RoY NoiseCleaner", false, [] { return std::make_unique<RoyNoiseCleaner>(); });
    f.add("roy.analyzer", "RoY Analyzer", false, [] { return std::make_unique<RoyAnalyzer>(); });
    f.add("roy.dynamicspace", "RoY Dynamic Space", false, [] { return std::make_unique<RoyDynamicSpace>(); });
}

} // namespace roy
