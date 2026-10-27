#include "raycast.h"
#include <algorithm>
#include <cmath>

float nearPlane(float sceneRadius) { return sceneRadius * 0.0012f; }

Vec3 viewRay(const Camera& cam, VecI2 canvas, VecI2 at) {
    float w = (float)std::max(1, canvas.x), h = (float)std::max(1, canvas.y);
    float nx = (2.0f * at.x / w - 1.0f), ny = (1.0f - 2.0f * at.y / h);
    float t = std::tan(kFovY * 0.5f);
    Vec3 f = cam.forward();
    Vec3 r = normalize(cross(Vec3(0, 1, 0), f));
    Vec3 u = cross(f, r);
    return normalize(f + r * (nx * t * w / h) + u * (ny * t));
}

bool raycastScene(const Scene& s, bool cullBackfaces, const Vec3& o, const Vec3& d, float& tOut, Vec3* normalOut) {
    float best = 1e30f;
    // The pipeline is left-handed (view z forward) with GL_CW front faces: a face is visible from the front when
    // dot(e1 x e2, d) < 0, which is det > 0 below.
    for (const Batch& b : s.batches) {
        if (b.blend == BLEND_ADD) continue;
        const bool cullThis = cullBackfaces && !b.twoSided;
        for (size_t i = 0; i + 2 < b.idx.size(); i += 3) {
            const SceneVertex &a = b.verts[b.idx[i]], &bb = b.verts[b.idx[i + 1]], &c = b.verts[b.idx[i + 2]];
            Vec3 p0(a.x, a.y, a.z), e1 = Vec3(bb.x, bb.y, bb.z) - p0, e2 = Vec3(c.x, c.y, c.z) - p0;
            Vec3 pv = cross(d, e2);
            float det = dot(e1, pv);
            if (std::fabs(det) < 1e-9f) continue;
            if (cullThis && det < 0) continue;
            float inv = 1.0f / det;
            Vec3 tv = o - p0;
            float u = dot(tv, pv) * inv;
            if (u < 0 || u > 1) continue;
            Vec3 qv = cross(tv, e1);
            float v = dot(d, qv) * inv;
            if (v < 0 || u + v > 1) continue;
            float t = dot(e2, qv) * inv;
            if (t > 0 && t < best) {
                best = t;
                if (normalOut) *normalOut = normalize(cross(e1, e2));
            }
        }
    }
    tOut = best;
    return best < 1e29f;
}

PivotPick pickPivotPoint(const Scene* scene, bool cullBackfaces, const Camera& cam, VecI2 canvas, VecI2 at,
                         const Vec3& sceneCenter, float sceneRadius) {
    PivotPick pick;
    Vec3 d = viewRay(cam, canvas, at);
    float t0 = nearPlane(sceneRadius) / std::max(1e-3f, dot(d, cam.forward()));
    Vec3 origin = cam.pos + d * t0;
    float t;
    Vec3 hitNormal(0, 1, 0);
    if (scene && raycastScene(*scene, cullBackfaces, origin, d, t, &hitNormal)) {
        pick.point = origin + d * t;
        pick.normalY = std::fabs(hitNormal.y);
    } else if (d.y < -0.01f) {  // the ground / scene-centre fallbacks count as horizontal
        float ty = (sceneCenter.y - cam.pos.y) / d.y;
        pick.point = cam.pos + d * std::max(ty, sceneRadius * 0.05f);
    } else {
        pick.point = cam.pos + d * sceneRadius * 0.5f;
    }
    return pick;
}
