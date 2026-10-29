#include "minigame01/collision_world.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

// Closest point of triangle (a, a+e1, a+e2) to p (Ericson, Real-Time Collision Detection).
Vec3f closestOnTriangle(const Vec3f& p, const Vec3f& a, const Vec3f& e1, const Vec3f& e2) {
    Vec3f b = a + e1, c = a + e2;
    Vec3f ab = e1, ac = e2, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    Vec3f bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    Vec3f cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Slab test: does the ray o + t d, 0 <= t <= tMax, pass through the box?
bool rayHitsBox(const Vec3f& o, const Vec3f& d, const Vec3f& lo, const Vec3f& hi, float tMax) {
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    float t0 = 0, t1 = tMax;
    for (int k = 0; k < 3; k++) {
        if (std::fabs(dd[k]) < 1e-12f) {
            if (oo[k] < l[k] || oo[k] > h[k]) return false;
            continue;
        }
        float inv = 1.0f / dd[k], a = (l[k] - oo[k]) * inv, b = (h[k] - oo[k]) * inv;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

float distanceSquaredToBox(const Vec3f& p, const Vec3f& lo, const Vec3f& hi) {
    float dx = std::max(std::max(lo.x - p.x, 0.0f), p.x - hi.x);
    float dy = std::max(std::max(lo.y - p.y, 0.0f), p.y - hi.y);
    float dz = std::max(std::max(lo.z - p.z, 0.0f), p.z - hi.z);
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

template <class F>
void CollisionWorld::forCells(const Vec3f& bmin, const Vec3f& bmax, F&& fn) const {
    if (nx == 0) return;
    int x0 = std::max(0, (int)std::floor((bmin.x - lo.x) / cell)), x1 = std::min(nx - 1, (int)std::floor((bmax.x - lo.x) / cell));
    int z0 = std::max(0, (int)std::floor((bmin.z - lo.z) / cell)), z1 = std::min(nz - 1, (int)std::floor((bmax.z - lo.z) / cell));
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            int c = z * nx + x;
            for (int k = cellStart[c]; k < cellStart[c + 1]; k++) fn(cellItems[k]);
        }
}

void CollisionWorld::clear() {
    cell = 6.0f;
    tris.clear();
    large.clear();
    cellStart.clear();
    cellItems.clear();
    bvhs.clear();
    nx = nz = 0;
}

void CollisionWorld::build(const std::vector<CollisionMesh>& meshes, float scale) {
    clear();
    bothSidesSolid = true;
    for (const CollisionMesh& m : meshes) {
        const bool tree = !m.nodes.empty();  // a smoothed mesh has none: its triangles go into the grid
        Bvh bv;
        bv.root = m.root;
        bv.polyFirst.reserve(m.polys.size() + 1);
        for (const CollisionMesh::Poly& p : m.polys) {
            bv.polyFirst.push_back((int)bv.tris.size());
            const uint32_t* c = &m.corners[p.first];
            for (int k = 2; k < p.n; k++) {  // a fan: (0 1 2), (0 2 3), ...
                Tri t;
                t.twoSided = true;
                t.a = m.verts[c[0]] * scale;
                t.e1 = m.verts[c[k - 1]] * scale - t.a;
                t.e2 = m.verts[c[k]] * scale - t.a;
                Vec3f n = cross(t.e1, t.e2);
                float l = length(n);
                if (l < 1e-7f) continue;
                t.n = n * (1.0f / l);
                (tree ? bv.tris : tris).push_back(t);
            }
        }
        if (!tree) continue;
        bv.polyFirst.push_back((int)bv.tris.size());
        bv.nodes.reserve(m.nodes.size());
        for (const CollisionMesh::Node& nd : m.nodes) {
            Bvh::Node n;
            n.lo = (nd.centre - nd.half) * scale;
            n.hi = (nd.centre + nd.half) * scale;
            n.child[0] = nd.child[0];
            n.child[1] = nd.child[1];
            bv.nodes.push_back(n);
        }
        if (!bv.tris.empty()) bvhs.push_back(std::move(bv));
    }
    buildGrid();
}

void CollisionWorld::build(const Scene& scene, float scale, bool bothSides) {
    clear();
    bothSidesSolid = bothSides;
    for (const Batch& b : scene.batches) {
        for (size_t i = 0; i + 2 < b.idx.size(); i += 3) {
            const SceneVertex &va = b.verts[b.idx[i]], &vb = b.verts[b.idx[i + 1]], &vc = b.verts[b.idx[i + 2]];
            Tri t;
            t.twoSided = b.twoSided;
            t.a =Vec3f(va.x, va.y, va.z) * scale;
            t.e1 = Vec3f(vb.x, vb.y, vb.z) * scale - t.a;
            t.e2 = Vec3f(vc.x, vc.y, vc.z) * scale - t.a;
            Vec3f n = cross(t.e1, t.e2);
            float l = length(n);
            if (l < 1e-7f) continue;
            t.n = n * (1.0f / l);
            tris.push_back(t);
        }
    }
    buildGrid();
}

// The uniform grid over `tris`.
void CollisionWorld::buildGrid() {
    if (tris.empty()) return;
    // grid bounds from the triangles that are small enough to live in it
    const float kLarge = 80.0f;
    lo = Vec3f(1e30f, 1e30f, 1e30f);
    hi = Vec3f(-1e30f, -1e30f, -1e30f);
    std::vector<char> isLarge(tris.size(), 0);
    for (size_t i = 0; i < tris.size(); i++) {
        const Tri& t = tris[i];
        Vec3f mn = vmin(t.a, vmin(t.a + t.e1, t.a + t.e2)), mx = vmax(t.a, vmax(t.a + t.e1, t.a + t.e2));
        if (mx.x - mn.x > kLarge || mx.z - mn.z > kLarge) { isLarge[i] = 1; large.push_back((int)i); continue; }
        lo = vmin(lo, mn);
        hi = vmax(hi, mx);
    }
    if (lo.x > hi.x) return;  // nothing but large triangles
    nx = std::max(1, (int)std::ceil((hi.x - lo.x) / cell) + 1);
    nz = std::max(1, (int)std::ceil((hi.z - lo.z) / cell) + 1);
    while ((long long)nx * nz > 1500000) {
        cell *= 1.5f;
        nx = (int)std::ceil((hi.x - lo.x) / cell) + 1;
        nz = (int)std::ceil((hi.z - lo.z) / cell) + 1;
    }
    std::vector<int> count((size_t)nx * nz + 1, 0);
    auto cellRange = [&](const Tri& t, int& x0, int& x1, int& z0, int& z1) {
        Vec3f mn = vmin(t.a, vmin(t.a + t.e1, t.a + t.e2)), mx = vmax(t.a, vmax(t.a + t.e1, t.a + t.e2));
        x0 = std::max(0, (int)std::floor((mn.x - lo.x) / cell));
        x1 = std::min(nx - 1, (int)std::floor((mx.x - lo.x) / cell));
        z0 = std::max(0, (int)std::floor((mn.z - lo.z) / cell));
        z1 = std::min(nz - 1, (int)std::floor((mx.z - lo.z) / cell));
    };
    for (size_t i = 0; i < tris.size(); i++) {
        if (isLarge[i]) continue;
        int x0, x1, z0, z1;
        cellRange(tris[i], x0, x1, z0, z1);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) count[(size_t)z * nx + x + 1]++;
    }
    for (size_t c = 1; c < count.size(); c++) count[c] += count[c - 1];
    cellStart = count;
    cellItems.assign((size_t)cellStart.back(), 0);
    std::vector<int> fill(cellStart.begin(), cellStart.end() - 1);
    for (size_t i = 0; i < tris.size(); i++) {
        if (isLarge[i]) continue;
        int x0, x1, z0, z1;
        cellRange(tris[i], x0, x1, z0, z1);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) cellItems[(size_t)fill[(size_t)z * nx + x]++] = (int)i;
    }
}

bool CollisionWorld::raycast(const Vec3f& o, const Vec3f& d, float maxT, bool bothSides, float& tOut, Vec3f& normalOut) const {
    float best = maxT;
    bool hit = false;
    auto test = [&](const Tri& t) {
        Vec3f pv = cross(d, t.e2);
        float det = dot(t.e1, pv);
        if (std::fabs(det) < 1e-9f) return;
        if (!bothSides && !bothSidesSolid && !t.twoSided && det < 0) return;  // back face (same rule as the renderer's culling)
        float inv = 1.0f / det;
        Vec3f tv = o - t.a;
        float u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) return;
        Vec3f qv = cross(tv, t.e1);
        float v = dot(d, qv) * inv;
        if (v < 0 || u + v > 1) return;
        float tt = dot(t.e2, qv) * inv;
        if (tt > 1e-4f && tt < best) {
            best = tt;
            normalOut = det > 0 ? t.n : t.n * -1.0f;
            hit = true;
        }
    };
    Vec3f end = o + d * maxT;
    forCells(vmin(o, end), vmax(o, end), [&](int ti) { test(tris[ti]); });
    for (int ti : large) test(tris[ti]);
    // the game's hierarchies: boxes the ray misses (within the nearest hit so far) are skipped with everything below them
    for (const Bvh& bv : bvhs) {
        int stack[128], sp = 0;
        stack[sp++] = bv.root;
        while (sp > 0) {
            int id = stack[--sp];
            if (id < 0) {
                for (int k = bv.polyFirst[~id]; k < bv.polyFirst[~id + 1]; k++) test(bv.tris[k]);
                continue;
            }
            const Bvh::Node& n = bv.nodes[id];
            if (!rayHitsBox(o, d, n.lo, n.hi, best) || sp > 126) continue;
            stack[sp++] = n.child[0];
            stack[sp++] = n.child[1];
        }
    }
    if (hit) tOut = best;
    return hit;
}

bool CollisionWorld::sphereContact(const Vec3f& c, float r, Vec3f& normalOut, float& depthOut) const {
    float bestDepth = 0;
    bool hit = false;
    auto test = [&](const Tri& t) {
        Vec3f q = closestOnTriangle(c, t.a, t.e1, t.e2);
        Vec3f diff = c - q;
        float d2 = dot(diff, diff);
        if (d2 >= r * r) return;
        float d = std::sqrt(d2);
        float depth = r - d;
        if (depth <= bestDepth) return;
        bestDepth = depth;
        normalOut = d > 1e-5f ? diff * (1.0f / d) : t.n;
        hit = true;
    };
    Vec3f ext(r, r, r);
    forCells(c - ext, c + ext, [&](int ti) { test(tris[ti]); });
    for (int ti : large) test(tris[ti]);
    for (const Bvh& bv : bvhs) {  // only the boxes the sphere reaches
        int stack[128], sp = 0;
        stack[sp++] = bv.root;
        while (sp > 0) {
            int id = stack[--sp];
            if (id < 0) {
                for (int k = bv.polyFirst[~id]; k < bv.polyFirst[~id + 1]; k++) test(bv.tris[k]);
                continue;
            }
            const Bvh::Node& n = bv.nodes[id];
            if (distanceSquaredToBox(c, n.lo, n.hi) >= r * r || sp > 126) continue;
            stack[sp++] = n.child[0];
            stack[sp++] = n.child[1];
        }
    }
    if (hit) depthOut = bestDepth;
    return hit;
}

}  // namespace minigame01
