#pragma once
// GMB 13.6 - ROUND-TRIP LATENCY MEASUREMENT (output -> cable / speaker+mic -> input).
//
// The engine plays a probe (three 100 ms pseudo-noise bursts at -12 dBFS, 0.6 s apart) on all outputs
// and records the input from the SAME callback on (AudioEngine::startLatencyProbe), so "sample i of
// the probe" and "sample i of the recording" belong to the same device callback position. The lag of
// the best normalised cross-correlation of each burst is the round-trip latency in samples
// (driver + buffers + converters + cable). Three bursts must agree within 2 samples, otherwise the
// measurement is rejected (noise, no loopback, echo) - no value is invented.
#include <string>
#include <vector>

namespace roy::diag {

struct LatencyResult {
    bool ok = false;
    int samples = 0;          // round-trip latency
    double ms = 0;
    double confidence = 0;    // smallest normalised correlation peak of the bursts (0..1)
    int spread = 0;           // max - min lag over the bursts
    std::vector<int> lags;    // per burst
    std::string message;
};

std::vector<float> makeLatencyProbe(double sampleRate);
double latencyProbeSeconds(); // how long to record (probe + up to 0.5 s latency)
LatencyResult analyseLatencyProbe(const std::vector<float>& probe, const std::vector<float>& recorded, double sampleRate,
                                  double maxLatencySeconds = 0.5);

} // namespace roy::diag
