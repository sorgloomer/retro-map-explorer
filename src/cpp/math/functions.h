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

