// Minimal vector / matrix math. Left-handed, Y-up, column-major matrices (GL layout).
#pragma once
#include <cmath>
#include <algorithm>
#include "./vec.h"

struct Transform3f {
    float m[12];  // m[col * 4 + row]

    static Transform3f identity() {
        Transform3f r{};
        for (int i = 0; i < 12; i++) r.m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        return r;
    }
    static Transform3f mul(const Transform3f& a, const Transform3f& b) {  // a * b
        Transform3f r{};
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }

    Vec3f translation() const {
        return {m[3], m[7], m[11]};
    }

    Vec3f transformPoint(const Vec3f& p) const {
        return {m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9],
                m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10],
                m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11]};
    }
    // The rotation / scale part only (directions, normals of a matrix without shear).
    Vec3f transformDirection(const Vec3f& d) const {
        return {m[0] * d.x + m[3] * d.y + m[6] * d.z,
                m[1] * d.x + m[4] * d.y + m[7] * d.z,
                m[2] * d.x + m[5] * d.y + m[8] * d.z};
    }
    static Transform3f lookAt(const Vec3f& eye, const Vec3f& fwd, const Vec3f& upHint) {
        Vec3f z = normalize(fwd);
        Vec3f x = normalize(cross(upHint, z));
        Vec3f y = cross(z, x);
        Transform3f r = identity();
        r.m[0] = x.x; r.m[3] = x.y; r.m[6] = x.z;
        r.m[1] = y.x; r.m[4] = y.y; r.m[7] = y.z;
        r.m[2] = z.x; r.m[5] = z.y; r.m[8] = z.z;
        r.m[9] = -dot(x, eye);
        r.m[10] = -dot(y, eye);
        r.m[11] = -dot(z, eye);
        return r;
    }
};

struct Mat4f {
    float m[16];  // m[col * 4 + row]

    Mat4f() = default;

    Mat4f(const Transform3f& t) : m{
        t.m[0], t.m[1], t.m[2], 0.0f,
        t.m[3], t.m[4], t.m[5], 0.0f,
        t.m[6], t.m[7], t.m[8], 0.0f,
        t.m[9], t.m[10], t.m[11], 1.0f,
    } {
    }

    static Mat4f identity() {
        Mat4f r{};
        for (int i = 0; i < 16; i++) r.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return r;
    }
    static Mat4f mul(const Mat4f& a, const Mat4f& b) {  // a * b
        Mat4f r{};
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec3f transformPoint(const Vec3f& p) const {
        const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        Vec3f q = {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
        return q * (1.0f / w);
    }
    // The rotation / scale part only (directions, normals of a matrix without shear).
    Vec3f transformDirection(const Vec3f& d) const {
        return {m[0] * d.x + m[4] * d.y + m[8] * d.z,
                m[1] * d.x + m[5] * d.y + m[9] * d.z,
                m[2] * d.x + m[6] * d.y + m[10] * d.z};
    }
    // Left-handed perspective, clip z in [-1, 1].
    static Mat4f perspective(float fovY, float aspect, float zn, float zf) {
        Mat4f r{};
        for (float& v : r.m) v = 0;
        float f = 1.0f / std::tan(fovY * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zf + zn) / (zf - zn);
        r.m[11] = 1.0f;
        r.m[14] = -2.0f * zf * zn / (zf - zn);
        return r;
    }
};
