// Carmageddon scene formats: .hie hierarchies, .mshs / .msh meshes and .lin / .lins splines.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "vmath.h"

namespace carma {

constexpr bool kMirrorX = true;  // the game is left-handed: mirror X so the maps read like in-game

inline Vec3 fixX(Vec3 v) { if (kMirrorX) v.x = -v.x; return v; }
inline float fixA(float a) { return kMirrorX ? -a : a; }  // heading angle (sin a, cos a) of a mirrored direction

struct Hie {
    struct Mat { Mat4 m; std::string name; };
    struct Node { int type, index, child, sibling; };
    std::vector<std::string> textures, meshNames, lineNames, collNames;
    std::vector<int> matFlags;  // flags of the "materials" section; bit 8 = additive (glowing) surfaces
    std::vector<Mat> mats;
    std::vector<Node> nodes;

    static bool section(const std::vector<std::string>& L, const char* key, std::vector<std::string>& items);

    static std::string unquote(const std::string& s);

    bool parse(const std::string& text);
};

struct CMesh {
    std::vector<float> v;  // 9 floats per vertex: pos(3) rgba(4) uv(2)
    std::vector<uint32_t> idx;
    size_t nv() const { return v.size() / 9; }
};

// Appends every mesh of a .mshs file (regular, NGon polygon and per-triangle layouts). False when the data is unreadable.
bool parseMshs(const std::vector<uint8_t>& data, std::vector<CMesh>& out);
// Appends the splines of a .lins file (or the single spline of a text .lin).
bool parseLins(const std::vector<uint8_t>& data, std::vector<std::vector<Vec3>>& out);

}  // namespace carma
