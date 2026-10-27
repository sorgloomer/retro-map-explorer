// Two-component vectors: Vec2 (float) and VecI2 (integer: pixel positions, sizes).
#pragma once
#include <cmath>

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
};
inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float length(const Vec2& a) { return std::sqrt(dot(a, a)); }

struct VecI2 {
    int x = 0, y = 0;
    VecI2() = default;
    VecI2(int x_, int y_) : x(x_), y(y_) {}
    explicit VecI2(const Vec2& v) : x((int)v.x), y((int)v.y) {}  // truncates
    explicit operator Vec2() const { return {(float)x, (float)y}; }
    VecI2 operator+(const VecI2& o) const { return {x + o.x, y + o.y}; }
    VecI2 operator-(const VecI2& o) const { return {x - o.x, y - o.y}; }
    bool operator==(const VecI2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const VecI2& o) const { return !(*this == o); }
};
