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

    static Vec2f zero() { return {0.0f, 0.0f }; }
};
inline float dot(const Vec2f& a, const Vec2f& b) { return a.x * b.x + a.y * b.y; }
inline float length(const Vec2f& a) { return std::sqrt(dot(a, a)); }

struct Vec2i {
    int x = 0, y = 0;
    Vec2i() = default;
    Vec2i(int x_, int y_) : x(x_), y(y_) {}
    explicit Vec2i(const Vec2f& v) : x((int)v.x), y((int)v.y) {}  // truncates
    explicit operator Vec2f() const { return {(float)x, (float)y}; }
    Vec2i operator+(const Vec2i& o) const { return {x + o.x, y + o.y}; }
    Vec2i operator-(const Vec2i& o) const { return {x - o.x, y - o.y}; }
    bool operator==(const Vec2i& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2i& o) const { return !(*this == o); }

    static Vec2i zero() { return {0, 0}; }
};
