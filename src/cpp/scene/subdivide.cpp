#include "scene/subdivide.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

// Vertices that sit at the same place are one vertex of the surface, whatever their texture coordinates or colours say:
// meshes keep a copy per face (or per material), the surface they form is connected all the same.
struct Key3 {
    int32_t x, y, z;
    bool operator==(const Key3& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Key3Hash {
    size_t operator()(const Key3& k) const {
        uint64_t h = (uint64_t)(uint32_t)k.x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)k.y * 0xC2B2AE3D27D4EB4Full ^
                     (uint64_t)(uint32_t)k.z * 0x165667B19E3779F9ull;
        return (size_t)(h ^ (h >> 29));
    }
};

class Welder {
public:
    std::vector<Vec3f> pos;  // one per distinct place
    uint32_t id(const Vec3f& p) {
        Key3 k{(int32_t)std::lround(p.x * 100.0f), (int32_t)std::lround(p.y * 100.0f), (int32_t)std::lround(p.z * 100.0f)};
        auto it = ids.find(k);
        if (it != ids.end()) return it->second;
        uint32_t n = (uint32_t)pos.size();
        ids.emplace(k, n);
        pos.push_back(p);
        return n;
    }

private:
    std::unordered_map<Key3, uint32_t, Key3Hash> ids;
};

uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | b;
}

// The Loop rules over a connected triangle set: where the new vertices go and where the old ones move to.
class LoopSmoother {
public:
    LoopSmoother(const std::vector<Vec3f>& positions, float creaseCos) : pos(positions), creaseCos(creaseCos) {}

    void addTriangle(uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c) return;
        Vec3f n = cross(pos[b] - pos[a], pos[c] - pos[a]);
        float l = length(n);
        n = l > 1e-12f ? n * (1.0f / l) : Vec3f(0, 0, 0);
        addEdge(a, b, c, n);
        addEdge(b, c, a, n);
        addEdge(c, a, b, n);
    }

    void finish() {
        const size_t nv = pos.size();
        std::vector<Vec3f> smoothSum(nv, Vec3f(0, 0, 0)), sharpSum(nv, Vec3f(0, 0, 0));
        std::vector<int> smoothCount(nv, 0), sharpCount(nv, 0);
        for (auto& kv : edges) {
            Edge& e = kv.second;
            const uint32_t a = (uint32_t)(kv.first >> 32), b = (uint32_t)(kv.first & 0xffffffffu);
            // an edge keeps its shape when it is a border, shared by more than two faces, or a crease
            e.sharp = e.count != 2 || dot(e.n[0], e.n[1]) < creaseCos;
            if (e.sharp) {
                e.mid = (pos[a] + pos[b]) * 0.5f;
                sharpSum[a] = sharpSum[a] + pos[b]; sharpCount[a]++;
                sharpSum[b] = sharpSum[b] + pos[a]; sharpCount[b]++;
            } else {
                e.mid = (pos[a] + pos[b]) * 0.375f + (pos[e.opp[0]] + pos[e.opp[1]]) * 0.125f;
                smoothSum[a] = smoothSum[a] + pos[b]; smoothCount[a]++;
                smoothSum[b] = smoothSum[b] + pos[a]; smoothCount[b]++;
            }
        }
        moved.resize(nv);
        for (size_t v = 0; v < nv; v++) {
            if (sharpCount[v] == 0 && smoothCount[v] > 0) {  // an inner vertex
                const float n = (float)smoothCount[v];
                const float beta = smoothCount[v] == 3 ? 3.0f / 16.0f : 3.0f / (8.0f * n);
                moved[v] = pos[v] * (1.0f - n * beta) + smoothSum[v] * beta;
            } else if (sharpCount[v] == 2) {  // on a crease or a border: slides along it
                moved[v] = pos[v] * 0.75f + sharpSum[v] * 0.125f;
            } else {  // a corner (or alone): stays
                moved[v] = pos[v];
            }
        }
    }

    Vec3f vertexPos(uint32_t v) const { return moved[v]; }
    // The new vertex in the middle of edge a-b; `fallback` when the edge is not part of the surface.
    Vec3f edgePos(uint32_t a, uint32_t b, const Vec3f& fallback) const {
        auto it = edges.find(edgeKey(a, b));
        return it == edges.end() ? fallback : it->second.mid;
    }

private:
    struct Edge {
        int count = 0;
        uint32_t opp[2] = {0, 0};  // the vertex across from the edge in its first two faces
        Vec3f n[2];
        bool sharp = false;
        Vec3f mid;
    };
    void addEdge(uint32_t a, uint32_t b, uint32_t opposite, const Vec3f& n) {
        Edge& e = edges[edgeKey(a, b)];
        if (e.count < 2) { e.opp[e.count] = opposite; e.n[e.count] = n; }
        e.count++;
    }

    const std::vector<Vec3f>& pos;
    float creaseCos;
    std::unordered_map<uint64_t, Edge> edges;
    std::vector<Vec3f> moved;
};

SceneVertex mixVertex(const SceneVertex& a, const SceneVertex& b) {
    SceneVertex m;
    m.x = (a.x + b.x) * 0.5f; m.y = (a.y + b.y) * 0.5f; m.z = (a.z + b.z) * 0.5f;
    m.r = (uint8_t)((a.r + b.r + 1) / 2); m.g = (uint8_t)((a.g + b.g + 1) / 2);
    m.b = (uint8_t)((a.b + b.b + 1) / 2); m.a = (uint8_t)((a.a + b.a + 1) / 2);
    m.u = (a.u + b.u) * 0.5f; m.v = (a.v + b.v) * 0.5f;
    return m;
}

}  // namespace

static size_t triangleCount(const Scene& s) {
    size_t n = 0;
    for (const Batch& b : s.batches) n += b.idx.size() / 3;
    return n;
}

static size_t triangleCount(const CollisionMesh& m) {
    size_t n = 0;
    for (const CollisionMesh::Poly& p : m.polys) n += p.n - 2;
    return n;
}

// One round of subdivision of the level's batches.
static void smoothScenePass(Scene& s, float creaseDegrees) {
    Welder weld;
    std::vector<std::vector<uint32_t>> cid(s.batches.size());
    for (size_t b = 0; b < s.batches.size(); b++) {
        cid[b].resize(s.batches[b].verts.size());
        for (size_t i = 0; i < cid[b].size(); i++) {
            const SceneVertex& v = s.batches[b].verts[i];
            cid[b][i] = weld.id(Vec3f(v.x, v.y, v.z));
        }
    }
    // one surface for the whole level: faces of different materials meet without cracks or borders
    LoopSmoother sm(weld.pos, std::cos(creaseDegrees * 3.14159265f / 180.0f));
    for (size_t b = 0; b < s.batches.size(); b++) {
        const Batch& bt = s.batches[b];
        for (size_t i = 0; i + 2 < bt.idx.size(); i += 3) sm.addTriangle(cid[b][bt.idx[i]], cid[b][bt.idx[i + 1]], cid[b][bt.idx[i + 2]]);
    }
    sm.finish();

    size_t triangles = 0;
    for (size_t b = 0; b < s.batches.size(); b++) {
        Batch& bt = s.batches[b];
        const size_t nt = bt.idx.size() / 3;
        std::vector<SceneVertex> verts;
        std::vector<uint32_t> idx;
        verts.reserve(nt * 6);
        idx.reserve(nt * 12);
        for (size_t t = 0; t < nt; t++) {
            const uint32_t i[3] = {bt.idx[t * 3], bt.idx[t * 3 + 1], bt.idx[t * 3 + 2]};
            const uint32_t c[3] = {cid[b][i[0]], cid[b][i[1]], cid[b][i[2]]};
            SceneVertex out[6];
            for (int k = 0; k < 3; k++) {  // the corners keep their colour and texture coordinates and move
                out[k] = bt.verts[i[k]];
                const Vec3f p = sm.vertexPos(c[k]);
                out[k].x = p.x; out[k].y = p.y; out[k].z = p.z;
            }
            static const int ends[3][2] = {{0, 1}, {1, 2}, {2, 0}};
            for (int k = 0; k < 3; k++) {  // the edge midpoints: 3, 4, 5
                SceneVertex m = mixVertex(bt.verts[i[ends[k][0]]], bt.verts[i[ends[k][1]]]);
                const Vec3f p = sm.edgePos(c[ends[k][0]], c[ends[k][1]], Vec3f(m.x, m.y, m.z));
                m.x = p.x; m.y = p.y; m.z = p.z;
                out[3 + k] = m;
            }
            const uint32_t base = (uint32_t)verts.size();
            verts.insert(verts.end(), out, out + 6);
            // corners 0 1 2, midpoints 3 (0-1) 4 (1-2) 5 (2-0): the winding is kept
            const uint32_t tri[12] = {0, 3, 5, 3, 1, 4, 5, 4, 2, 3, 4, 5};
            for (uint32_t k : tri) idx.push_back(base + k);
        }
        triangles += nt * 4;
        bt.verts = std::move(verts);
        bt.idx = std::move(idx);
        bt.normals.clear();
    }
    s.triCount = triangles;
}

// One round of subdivision of a collision mesh.
static void smoothCollisionPass(CollisionMesh& m, float creaseDegrees) {
    Welder weld;
    std::vector<uint32_t> cid(m.verts.size());
    for (size_t i = 0; i < cid.size(); i++) cid[i] = weld.id(m.verts[i]);
    struct Tri { uint32_t a, b, c; uint8_t material; };
    std::vector<Tri> tris;
    for (const CollisionMesh::Poly& p : m.polys)
        for (int k = 2; k < p.n; k++) tris.push_back({cid[m.corners[p.first]], cid[m.corners[p.first + k - 1]], cid[m.corners[p.first + k]], p.material});
    LoopSmoother sm(weld.pos, std::cos(creaseDegrees * 3.14159265f / 180.0f));
    for (const Tri& t : tris) sm.addTriangle(t.a, t.b, t.c);
    sm.finish();

    CollisionMesh out;
    out.verts.resize(weld.pos.size());
    for (size_t v = 0; v < weld.pos.size(); v++) out.verts[v] = sm.vertexPos((uint32_t)v);
    std::unordered_map<uint64_t, uint32_t> mid;  // edge -> its new vertex (shared by the faces next to it)
    auto edgeVertex = [&](uint32_t a, uint32_t b) {
        const uint64_t key = edgeKey(a, b);
        auto it = mid.find(key);
        if (it != mid.end()) return it->second;
        const uint32_t id = (uint32_t)out.verts.size();
        out.verts.push_back(sm.edgePos(a, b, (weld.pos[a] + weld.pos[b]) * 0.5f));
        mid.emplace(key, id);
        return id;
    };
    for (const Tri& t : tris) {
        if (t.a == t.b || t.b == t.c || t.a == t.c) continue;
        const uint32_t ab = edgeVertex(t.a, t.b), bc = edgeVertex(t.b, t.c), ca = edgeVertex(t.c, t.a);
        const uint32_t quad[4][3] = {{t.a, ab, ca}, {ab, t.b, bc}, {ca, bc, t.c}, {ab, bc, ca}};
        for (const auto& q : quad) {
            CollisionMesh::Poly p;
            p.first = (uint32_t)out.corners.size();
            p.n = 3;
            p.material = t.material;
            out.corners.insert(out.corners.end(), q, q + 3);
            out.polys.push_back(p);
        }
    }
    m = std::move(out);
}

int smoothScene(Scene& s, int passes, float creaseDegrees, size_t maxTriangles) {
    int done = 0;
    while (done < passes && triangleCount(s) * 4 <= maxTriangles) {
        smoothScenePass(s, creaseDegrees);
        done++;
    }
    return done;
}

int smoothCollisionMesh(CollisionMesh& m, int passes, float creaseDegrees, size_t maxTriangles) {
    int done = 0;
    while (done < passes && triangleCount(m) * 4 <= maxTriangles) {
        smoothCollisionPass(m, creaseDegrees);
        done++;
    }
    return done;
}
