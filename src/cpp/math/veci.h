// Two-component vectors: Vec2f (float) and Vec2i (integer: pixel positions, sizes).
#pragma once
#include <cmath>

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
