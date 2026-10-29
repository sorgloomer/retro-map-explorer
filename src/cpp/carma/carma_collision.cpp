// The level's static collision meshes (.scol): what the game's physics collides with, kept apart from the visible meshes.
//   u32 vertexCount, vertexCount * {f32 x, y, z}
//   u32 polygonCount, u32 total vertex references, u32 node count + 1 (polygons + nodes + 1 = ids in the tree)
//   polygonCount * { f32 centre[3], f32 half[3] (box), u32 flags, u32 n (3 or more: mostly 3 and 4, a few 5 and 6), u32 vertex[n] }
//       flags: low 16 bits = collision material (the order of the .h file next to it), high bits = per-edge flags
//   (polygonCount - 1) * { f32 centre[3], f32 half[3] (box), u32 childA, u32 childB }   a bounding volume hierarchy
//       a child id below polygonCount is that polygon, otherwise it is node (id - polygonCount); each node box is the exact
//       union of its children's boxes
//   u32 root id (the first node)
#include <cstring>
#include "carma/carma_game.h"
#include "vfs.h"

namespace carma {

namespace {

constexpr uint32_t kMaterialColours[8] = {0x4f7dff, 0x35d0e0, 0xb0793f, 0xb4b4b4, 0xf2d13d, 0x56c456, 0xe05ccf, 0xf08a30};

struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool u32(uint32_t& v) {
        if (p + 4 > d.size()) return false;
        memcpy(&v, &d[p], 4);
        p += 4;
        return true;
    }
    bool f32(float& v) {
        if (p + 4 > d.size()) return false;
        memcpy(&v, &d[p], 4);
        p += 4;
        return true;
    }
};

}  // namespace

// Parses one .scol (mirrored like the visible meshes). False when it is unreadable or its tree does not add up.
bool CarmaGame::parseCollisionMesh(const std::vector<uint8_t>& d, CollisionMesh& m) {
    Reader r{d};
    uint32_t nv = 0, np = 0, skip = 0;
    if (!r.u32(nv) || nv == 0 || r.p + (size_t)nv * 12 > d.size()) return false;
    m.verts.resize(nv);
    for (uint32_t i = 0; i < nv; i++) {
        float x, y, z;
        r.f32(x); r.f32(y); r.f32(z);
        m.verts[i] = Vec3f(kMirrorX ? -x : x, y, z);
    }
    if (!r.u32(np) || !r.u32(skip) || !r.u32(skip) || np == 0) return false;
    m.polys.resize(np);
    for (uint32_t k = 0; k < np; k++) {
        float box[6];
        uint32_t flags, n;
        for (float& f : box) if (!r.f32(f)) return false;
        if (!r.u32(flags) || !r.u32(n) || n < 3 || n > 64) return false;  // triangles and quads, a few levels have 5 and 6 sided ones
        CollisionMesh::Poly& poly = m.polys[k];
        poly.first = (uint32_t)m.corners.size();
        poly.n = (uint8_t)n;
        poly.material = (uint8_t)(flags & 0xff);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v;
            if (!r.u32(v) || v >= nv) return false;
            m.corners.push_back(v);
        }
    }
    // the tree: a node reference is a polygon below np, else a node
    const size_t nn = np - 1;
    if (r.p + nn * 32 + 4 > d.size()) return false;
    auto child = [&](uint32_t id, int& out) {
        if (id < np) { out = ~(int)id; return true; }
        if (id - np >= nn) return false;
        out = (int)(id - np);
        return true;
    };
    m.nodes.resize(nn);
    for (size_t i = 0; i < nn; i++) {
        float f[6];
        uint32_t a, b;
        for (float& v : f) r.f32(v);
        r.u32(a); r.u32(b);
        CollisionMesh::Node& nd = m.nodes[i];
        nd.centre = Vec3f(kMirrorX ? -f[0] : f[0], f[1], f[2]);
        nd.half = Vec3f(f[3], f[4], f[5]);
        if (!child(a, nd.child[0]) || !child(b, nd.child[1])) return false;
    }
    uint32_t rootId;
    if (!r.u32(rootId) || !child(rootId, m.root)) return false;
    return true;
}

// The collision mesh as a translucent overlay: coloured by surface type, quads split in two.
void CarmaGame::buildCollisionOverlay(const CollisionMesh& m, BatchSet& out) {
    Batch& b = out.batchFor(-1, BLEND_ALPHA, true);
    for (const CollisionMesh::Poly& poly : m.polys) {
        const uint32_t rgb = kMaterialColours[poly.material & 7];
        const uint32_t base = (uint32_t)b.verts.size();
        for (int i = 0; i < poly.n; i++) {
            const Vec3f& p = m.verts[m.corners[poly.first + i]];
            SceneVertex v;
            v.x = p.x; v.y = p.y; v.z = p.z;
            v.r = (uint8_t)(rgb >> 16); v.g = (uint8_t)(rgb >> 8); v.b = (uint8_t)rgb; v.a = 150;
            v.u = v.v = 0;
            b.verts.push_back(v);
        }
        for (int i = 2; i < poly.n; i++) b.idx.insert(b.idx.end(), {base, base + (uint32_t)i, base + (uint32_t)i - 1});  // a fan
    }
}

// Names the .scol files of the level's collision descriptors (the fourth column of the static mesh descriptor). Only the small
// text files are read here; the meshes themselves are read the first time the scene needs them (Scene::ensureCollision).
void CarmaGame::findCollision(const std::vector<std::string>& descriptors, Scene& out) {
    std::vector<std::string> files;
    for (const std::string& dn : descriptors) {
        std::string text;
        if (!readNamedText(dn, text)) continue;
        for (const std::string& line : splitLines(text))
            for (const std::string& s : quotedStrings(stripComment(line)))
                if (endsWith(vfs::lower(s), ".scol")) files.push_back(s);
    }
    out.collisionRequired = true;  // the minigame drives on these meshes only
    if (files.empty()) return;
    out.collisionLoader = [this, files](Scene& s) {
        for (const std::string& f : files) {
            std::vector<uint8_t> data;
            CollisionMesh m;
            if (!readNamed(f, data) || !parseCollisionMesh(data, m)) {
                s.collisionErrors.push_back(f);
                continue;
            }
            buildCollisionOverlay(m, s.collision);
            s.collisionMeshes.push_back(std::move(m));
        }
    };
}

}  // namespace carma
