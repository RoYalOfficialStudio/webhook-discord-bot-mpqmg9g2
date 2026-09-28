// MOCK stem-separation engine for the test-suite ONLY. It does not separate anything:
// it writes fixed fractions of the input so the AdvancedStemSeparator adapter (process
// launch, argument placeholders, output validation, residual, failure handling) can be
// tested without a real ML model.
//   roy_test_stem_engine <in.wav> <outdir> ok|fail|hang|missing|rate
#include "io/AudioFile.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace roy;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: roy_test_stem_engine <in.wav> <outdir> <mode>\n");
        return 2;
    }
    const std::string mode = argv[3];
    std::printf("mock engine: mode %s\n", mode.c_str());
    std::fflush(stdout);
    if (mode == "fail") {
        std::fprintf(stdout, "model file not found (mock failure)\n");
        return 3;
    }
    if (mode == "hang")
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    AudioData in;
    std::string err;
    if (!readAudioFile(argv[1], in, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 4;
    }
    auto scaled = [&](float g) {
        auto c = in.channels;
        for (auto& ch : c)
            for (auto& v : ch) v *= g;
        return c;
    };
    const std::string out = argv[2];
    const double sr = mode == "rate" ? 44100.0 : in.sampleRate;
    if (!writeWavFile(out + "/vocals.wav", scaled(0.5f), sr, SampleFormat::Float32, true, &err)) return 5;
    if (mode != "missing" && !writeWavFile(out + "/drums.wav", scaled(0.25f), sr, SampleFormat::Float32, true, &err)) return 5;
    for (int i = 0; i < 2000; ++i) std::printf("progress %d\n", i); // chatty engines must not block on a full pipe
    return 0;
}
