#pragma once


struct Quat {
    float w = 1, x = 0, y = 0, z = 0;

    Quat operator+(const Quat& other) const {
        return {
            .w = w + other.w,
            .x = x + other.x,
            .y = y + other.y,
            .z = z + other.z,
        };
    }

    Quat operator-(const Quat& other) const {
        return {
            .w = w - other.w,
            .x = x - other.x,
            .y = y - other.y,
            .z = z - other.z,
        };
    }

    Quat operator*(const float scale) const {
        return {
            .w = w * scale,
            .x = w * scale,
            .y = w * scale,
            .z = w * scale,
        };
    }

    Quat operator/(const float scale) const {
        return {
            .w = w / scale,
            .x = w / scale,
            .y = w / scale,
            .z = w / scale,
        };
    }

    Quat operator*(const Quat& other) const {
        return {
            .w = w * other.w - x * other.x - y * other.y - z * other.z,
            .x = w * other.x + x * other.w + y * other.z - z * other.y,
            .y = w * other.y - x * other.z + y * other.w + z * other.x,
            .z = w * other.z + x * other.y - y * other.x + z * other.w,
        };
    }
};

inline Vec3f rotate(const Quat& q, const Vec3f& v) {
    const Vec3f u(q.x, q.y, q.z);
    const Vec3f t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline Quat normalize(Quat q) {
    const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return l < 1e-9f ? Quat{} : q * (1.0f / l);
}
