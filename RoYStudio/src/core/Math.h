#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace roy {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

inline float dbToGain(float db) { return db <= -120.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline double dbToGain(double db) { return db <= -120.0 ? 0.0 : std::pow(10.0, db / 20.0); }
inline float gainToDb(float g, float floorDb = -120.0f) {
    return g <= 0.0f ? floorDb : std::max(floorDb, 20.0f * std::log10(g));
}
inline double gainToDb(double g, double floorDb = -120.0) {
    return g <= 0.0 ? floorDb : std::max(floorDb, 20.0 * std::log10(g));
}

inline double midiToHz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
inline double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

// Removes denormals / non-finite values. Cheap enough for per-sample state.
inline float sanitize(float x) {
    if (!std::isfinite(x)) return 0.0f;
    return std::fabs(x) < 1e-20f ? 0.0f : x;
}
inline double sanitize(double x) {
    if (!std::isfinite(x)) return 0.0;
    return std::fabs(x) < 1e-30 ? 0.0 : x;
}

// Equal-power pan law (-3 dB centre). pan in [-1, 1].
inline void panGains(float pan, float& left, float& right) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(kPi);
    left = std::cos(angle) * static_cast<float>(std::numbers::sqrt2);
    right = std::sin(angle) * static_cast<float>(std::numbers::sqrt2);
}

// Deterministic, allocation-free PRNG (xorshift64*) - usable on audio thread.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    double gaussian() {
        const double u1 = std::max(1e-12, uniform());
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
    }
};

// Scoped flush-to-zero / denormals-are-zero for x86 (audio threads).
class ScopedNoDenormals {
public:
    ScopedNoDenormals();
    ~ScopedNoDenormals();
private:
    unsigned int previous_ = 0;
};

} // namespace roy
