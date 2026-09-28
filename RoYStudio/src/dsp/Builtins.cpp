// Registration of all built-in RoY processors (effects and instruments).
#include "audio/Processor.h"

namespace roy {

void registerBuiltinProcessors() {
    static bool done = false;
    if (done) return;
    done = true;
}

} // namespace roy
