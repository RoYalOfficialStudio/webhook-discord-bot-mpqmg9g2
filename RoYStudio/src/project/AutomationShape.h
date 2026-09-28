#pragma once
// Automation segment shapes. A point's curve describes the segment from that point
// to the next one. Pure functions, no allocation: safe on the audio thread.
#include <algorithm>
#include <cmath>

namespace roy::automation {

enum Curve : int { Linear = 0, Hold = 1, Smooth = 2, Bezier = 3 };

inline const char* curveName(int c) {
    switch (c) {
    case Hold: return "Hold";
    case Smooth: return "Smooth";
    case Bezier: return "Bezier";
    default: return "Linear";
    }
}

// Fraction (0..1) of the way from the start value to the end value at x (0..1).
// Bezier: quadratic Bezier from (0,0) to (1,1) with the control point pulled towards
// (0,1) for tension > 0 (fast start) or (1,0) for tension < 0 (slow start).
inline double shape(int curve, double tension, double x) noexcept {
    x = std::clamp(x, 0.0, 1.0);
    switch (curve) {
    case Hold: return 0.0;
    case Smooth: return 0.5 - 0.5 * std::cos(3.14159265358979323846 * x);
    case Bezier: {
        const double t = std::clamp(tension, -1.0, 1.0);
        const double cx = 0.5 - 0.5 * t, cy = 0.5 + 0.5 * t;
        // solve x(s) = 2 s (1-s) cx + s^2 for s in [0,1]
        const double a = 1.0 - 2.0 * cx, b = 2.0 * cx;
        double s;
        if (std::fabs(a) < 1e-12) s = x / b;
        else s = (-b + std::sqrt(std::max(0.0, b * b + 4.0 * a * x))) / (2.0 * a);
        s = std::clamp(s, 0.0, 1.0);
        return 2.0 * s * (1.0 - s) * cy + s * s;
    }
    default: return x;
    }
}

inline double interpolate(double v0, double v1, int curve, double tension, double x) noexcept {
    return v0 + (v1 - v0) * shape(curve, tension, x);
}

} // namespace roy::automation
