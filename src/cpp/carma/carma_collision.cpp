// The level's static collision mesh (.scol): what the game's physics collides with, kept apart from the visible meshes.
//   u32 vertexCount, vertexCount * {f32 x, y, z}
//   u32 polygonCount, u32, u32 (a total index count and a node count of the lookup tree that follows the polygons)
//   polygonCount * { f32 centre[3], f32 halfExtents[3] (bounding box), u32 flags, u32 n (3 or 4), u32 vertex[n] }
//   flags: low 16 bits = collision material (the order of the .h file next to it), high bits = per-edge flags
//   ... the lookup tree (not read)
#include <cstring>
#include "carma/carma_game.h"
#include "vfs.h"

namespace carma {

namespace {

constexpr uint32_t kMaterialColours[8] = {0x4f7dff, 0x35d0e0, 0xb0793f, 0xb4b4b4, 0xf2d13d, 0x56c456, 0xe05ccf, 0xf08a30};

bool readU32(const std::vector<uint8_t>& d, size_t& p, uint32_t& v) {
    if (p + 4 > d.size()) return false;
    memcpy(&v, &d[p], 4);
    p += 4;
    return true;
}

}  // namespace

// Parses one .scol into the scene's collision set (coloured by material, mirrored like the visible meshes). False when unreadable.
bool CarmaGame::addCollisionMesh(const std::vector<uint8_t>& d) {
    size_t p = 0;
    uint32_t nv = 0, np = 0, skip = 0;
    if (!readU32(d, p, nv) || nv == 0 || p + (size_t)nv * 12 > d.size()) return false;
    std::vector<Vec3f> pts(nv);
    for (uint32_t i = 0; i < nv; i++) {
        float f[3];
        memcpy(f, &d[p], 12);
        p += 12;
        Vec3f w(f[0], f[1], f[2]);
        if (kMirrorX) w.x = -w.x;
        pts[i] = w;
    }
    if (!readU32(d, p, np) || !readU32(d, p, skip) || !readU32(d, p, skip)) return false;
    Batch& b = sc->collision.batchFor(-1, BLEND_ALPHA, true);
    for (uint32_t k = 0; k < np; k++) {
        if (p + 32 > d.size()) return false;
        uint32_t flags, n;
        p += 24;  // the bounding box
        readU32(d, p, flags);
        readU32(d, p, n);
        if ((n != 3 && n != 4) || p + (size_t)n * 4 > d.size()) return false;
        uint32_t id[4];
        for (uint32_t i = 0; i < n; i++) {
            readU32(d, p, id[i]);
            if (id[i] >= nv) return false;
        }
        const uint32_t rgb = kMaterialColours[(flags & 0xffff) & 7];
        const uint32_t base = (uint32_t)b.verts.size();
        for (uint32_t i = 0; i < n; i++) {
            SceneVertex v;
            v.x = pts[id[i]].x; v.y = pts[id[i]].y; v.z = pts[id[i]].z;
            v.r = (uint8_t)(rgb >> 16); v.g = (uint8_t)(rgb >> 8); v.b = (uint8_t)rgb; v.a = 150;
            v.u = v.v = 0;
            b.verts.push_back(v);
        }
        // the mirror reverses the winding
        b.idx.insert(b.idx.end(), {base, base + 2, base + 1});
        if (n == 4) b.idx.insert(b.idx.end(), {base, base + 3, base + 2});
    }
    return true;
}

// Every .scol the level's collision descriptors name (the fourth column of the static mesh descriptor).
void CarmaGame::readCollision(const std::vector<std::string>& descriptors) {
    for (const std::string& dn : descriptors) {
        std::string text;
        if (!readNamedText(dn, text)) continue;
        for (const std::string& line : splitLines(text))
            for (const std::string& s : quotedStrings(stripComment(line))) {
                if (!endsWith(vfs::lower(s), ".scol")) continue;
                std::vector<uint8_t> data;
                if (readNamed(s, data) && !addCollisionMesh(data)) missing.push_back(s + " (unreadable)");
            }
    }
}

}  // namespace carma
