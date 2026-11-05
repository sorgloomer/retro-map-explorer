#include "scene/mover_pose.h"
#include <algorithm>
#include <cmath>

bool moverPoseAt(const Mover& m, float time, Mat4f& out) {
    if (!m.keys.empty()) {  // animated prop: looping translate / rotate steps
        float total = 0;
        for (const AnimKey& k : m.keys) total += k.dur;
        float tt = total > 0 ? std::fmod(time, total) : 0;
        if (tt < 0) tt += total;
        Vec3f off;
        Mat4f R = Mat4f::identity();
        float start = 0;
        for (const AnimKey& k : m.keys) {
            float f = k.dur > 0 ? std::min(1.0f, std::max(0.0f, (tt - start) / k.dur)) : 1.0f;
            off = off + k.trans * f;
            if (k.rotDeg != 0) {
                float a = k.rotDeg * f * 0.0174532925f, cs = std::cos(a), sn = std::sin(a), t = 1 - cs;
                Vec3f ax = normalize(k.axis);
                Mat4f Q = Mat4f::identity();
                Q.m[0] = cs + ax.x * ax.x * t;      Q.m[4] = ax.x * ax.y * t - ax.z * sn; Q.m[8] = ax.x * ax.z * t + ax.y * sn;
                Q.m[1] = ax.y * ax.x * t + ax.z * sn; Q.m[5] = cs + ax.y * ax.y * t;      Q.m[9] = ax.y * ax.z * t - ax.x * sn;
                Q.m[2] = ax.z * ax.x * t - ax.y * sn; Q.m[6] = ax.z * ax.y * t + ax.x * sn; Q.m[10] = cs + ax.z * ax.z * t;
                R = Mat4f::mul(Q, R);
            }
            start += k.dur;
        }
        const Vec3f& c = m.pivot;
        R.m[12] = c.x - (R.m[0] * c.x + R.m[4] * c.y + R.m[8] * c.z) + off.x;
        R.m[13] = c.y - (R.m[1] * c.x + R.m[5] * c.y + R.m[9] * c.z) + off.y;
        R.m[14] = c.z - (R.m[2] * c.x + R.m[6] * c.y + R.m[10] * c.z) + off.z;
        out = R;
        return true;
    }
    if (m.length <= 0) return false;
    float d = std::fmod(m.speed * time + m.phase * m.length, m.length);
    if (d < 0) d += m.length;
    Vec3f pos, dir;
    if (!m.sample(d, pos, dir)) return false;
    // The game's followers (helicopter noses, energy arches) are modelled along local +X.
    Vec3f f = normalize(dir);
    Vec3f up = std::fabs(f.y) > 0.99f ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0);
    Vec3f z = normalize(cross(f, up));
    Vec3f u = cross(z, f);
    Mat4f M = Mat4f::identity();
    M.m[0] = f.x; M.m[1] = f.y; M.m[2] = f.z;
    M.m[4] = u.x; M.m[5] = u.y; M.m[6] = u.z;
    M.m[8] = z.x; M.m[9] = z.y; M.m[10] = z.z;
    M.m[12] = pos.x; M.m[13] = pos.y; M.m[14] = pos.z;
    out = M;
    return true;
}
