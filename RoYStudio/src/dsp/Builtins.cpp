// Registration of all built-in RoY processors (effects and instruments).
#include "audio/Processor.h"
#include "instruments/Synth.h"

namespace roy {

void registerBuiltinProcessors() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& f = ProcessorFactory::instance();
    f.add("roy.synth", "RoY Synth", true, [] { return std::make_unique<RoySynth>(); });
}

} // namespace roy
