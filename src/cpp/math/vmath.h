// Minimal vector / matrix math. Left-handed, Y-up, column-major matrices (GL layout).
#pragma once
#include <cmath>
#include <algorithm>

struct Vec3f {
    float x = 0, y = 0, z = 0;
    Vec3f() = default;
    Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3f& a) { return std::sqrt(dot(a, a)); }
inline Vec3f normalize(const Vec3f& a) {
    float l = length(a);
    return l > 1e-12f ? a * (1.0f / l) : Vec3f();
}
inline Vec3f vmin(const Vec3f& a, const Vec3f& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3f vmax(const Vec3f& a, const Vec3f& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Mat4 {
    float m[16];  // m[col * 4 + row]

    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 16; i++) r.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return r;
    }
    static Mat4 mul(const Mat4& a, const Mat4& b) {  // a * b
        Mat4 r{};
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec3f transformPoint(const Vec3f& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    // Left-handed perspective, clip z in [-1, 1].
    static Mat4 perspective(float fovY, float aspect, float zn, float zf) {
        Mat4 r{};
        for (float& v : r.m) v = 0;
        float f = 1.0f / std::tan(fovY * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zf + zn) / (zf - zn);
        r.m[11] = 1.0f;
        r.m[14] = -2.0f * zf * zn / (zf - zn);
        return r;
    }
    static Mat4 lookAt(const Vec3f& eye, const Vec3f& fwd, const Vec3f& upHint) {
        Vec3f z = normalize(fwd);
        Vec3f x = normalize(cross(upHint, z));
        Vec3f y = cross(z, x);
        Mat4 r = identity();
        r.m[0] = x.x; r.m[4] = x.y; r.m[8] = x.z;
        r.m[1] = y.x; r.m[5] = y.y; r.m[9] = y.z;
        r.m[2] = z.x; r.m[6] = z.y; r.m[10] = z.z;
        r.m[12] = -dot(x, eye);
        r.m[13] = -dot(y, eye);
        r.m[14] = -dot(z, eye);
        return r;
    }
};
