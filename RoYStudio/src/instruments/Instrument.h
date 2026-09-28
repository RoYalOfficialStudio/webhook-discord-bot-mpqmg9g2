#pragma once
// Base for sample-accurate instruments: splits the block at event offsets.
#include "audio/Processor.h"

#include <algorithm>

namespace roy {

class Instrument : public Processor {
public:
    using Processor::Processor;
    bool isInstrument() const override { return true; }
    void process(const AudioBlock& io, const AudioBlock*, const NoteEvent* events, int numEvents) noexcept override {
        beginBlock(io.numFrames);
        int pos = 0;
        for (int i = 0; i < numEvents; ++i) {
            const int at = std::clamp(events[i].offset, 0, io.numFrames);
            if (at > pos) {
                render(io, pos, at);
                pos = at;
            }
            handleEvent(events[i]);
        }
        if (pos < io.numFrames) render(io, pos, io.numFrames);
    }

protected:
    virtual void beginBlock(int numFrames) noexcept {}
    // ADDS output for frames [start, end).
    virtual void render(const AudioBlock& io, int start, int end) noexcept = 0;
    virtual void handleEvent(const NoteEvent& e) noexcept = 0;
};

} // namespace roy
