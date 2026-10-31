// Two-component vectors: Vec2f (float) and Vec2i (integer: pixel positions, sizes).
#pragma once
#include <cmath>

struct Vec2f {
    float x = 0, y = 0;
    Vec2f() = default;
    Vec2f(float x_, float y_) : x(x_), y(y_) {}
    Vec2f operator+(const Vec2f& o) const { return {x + o.x, y + o.y}; }
    Vec2f operator-(const Vec2f& o) const { return {x - o.x, y - o.y}; }
    Vec2f operator*(float s) const { return {x * s, y * s}; }
    Vec2f& operator+=(const Vec2f& o) { x += o.x; y += o.y; return *this; }

    static Vec2f zero() { return { 0.0f, 0.0f }; }
};

struct Vec3f {
    float x = 0, y = 0, z = 0;
    Vec3f() = default;
    Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }

    static Vec3f zero() { return { 0.0f, 0.0f, 0.0f }; }
};

inline float dot(const Vec2f& a, const Vec2f& b) { return a.x * b.x + a.y * b.y; }
inline float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline float length(const Vec2f& a) { return std::sqrt(dot(a, a)); }
inline float length(const Vec3f& a) { return std::sqrt(dot(a, a)); }

constexpr float EPS = 1e-12f;

inline Vec2f normalize(const Vec2f& a) {
    float l = length(a);
    return l > EPS ? a * (1.0f / l) : Vec2f::zero();
}

inline Vec3f normalize(const Vec3f& a) {
    float l = length(a);
    return l > EPS ? a * (1.0f / l) : Vec3f::zero();
}
inline Vec3f vmin(const Vec3f& a, const Vec3f& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3f vmax(const Vec3f& a, const Vec3f& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

inline Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
