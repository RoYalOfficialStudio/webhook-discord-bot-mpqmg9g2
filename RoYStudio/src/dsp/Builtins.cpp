// Registration of all built-in RoY processors (effects and instruments).
#include "audio/Processor.h"
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
}

} // namespace roy
