// The free camera: position plus yaw / pitch (radians). Yaw 0 looks along +Z, positive pitch looks up.
#pragma once
#include <cmath>
#include "../math/transform.h"

struct Camera {
    Vec3f pos;
    float yaw = 0, pitch = -0.8f;
    Vec3f forward() const { return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)}; }
};
