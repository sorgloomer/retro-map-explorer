// The free camera: position plus yaw / pitch (radians). Yaw 0 looks along +Z, positive pitch looks up.
#pragma once
#include <cmath>
#include "../math/vmath.h"

struct Camera {
    Vec3 pos;
    float yaw = 0, pitch = -0.8f;
    Vec3 forward() const { return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)}; }
};
