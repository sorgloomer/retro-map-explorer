#pragma once
#include <algorithm>

inline float clamp(const float x, const float xmin, const float xmax) {
    return std::max(xmin, std::min(xmax, x));
}

inline float smoothstep(const float x) {
    return x <= 0.0f ? 0.0f : x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x);
}

inline float smoothstep(const float xmin, const float xmax, const float x) {
    return smoothstep((x - xmin) / (xmax - xmin));
}

inline float mix(const float y0, const float y1, const float x) {
    return y0 * (1.0f - x) + y1  * x;
}

inline float smoothmin(const float x0, const float x1, const float kneewidth) {
    const float d = x1 - x0;
    return 0.5f * (x0 + x1 - std::sqrt(kneewidth * kneewidth + d * d));
}

inline float smoothlimit(const float x, const float xmax, const float kneefraction) {
    return smoothmin(x, xmax, xmax * kneefraction);
}
