// Static triangle collision for the minigame. Two ways to find the triangles near a query:
//   - a uniform grid over the visible level's triangles (games without collision data of their own), or
//   - the game's own collision meshes with their bounding volume hierarchy (a tree of boxes over the polygons).
#pragma once
#include <vector>
#include "../scene/scene.h"

namespace minigame01 {

class CollisionWorld {
public:
    // Takes every triangle of the level, whatever its blend mode or sidedness (bridges and domes are drawn translucent but solid).
    // Coordinates are multiplied by `scale` (to work in metres); bothSides: faces count from either side (games that draw double sided);
    // triangles of two-sided materials always do.
    void build(const Scene& scene, float scale = 1.0f, bool bothSides = false);
    // The game's collision meshes: solid from both sides, found through the meshes' own hierarchies.
    void build(const std::vector<CollisionMesh>& meshes, float scale);
    bool empty() const { return tris.empty() && bvhs.empty(); }

    // First triangle hit by the ray within maxT. Front faces only unless bothSides (or the triangle is two-sided); returns the unit normal of the hit face
    // (pointing against the ray).
    bool raycast(const Vec3f& origin, const Vec3f& dir, float maxT, bool bothSides, float& tOut, Vec3f& normalOut) const;

    // Deepest overlap of a sphere with the level: normal points from the surface towards the sphere centre.
    bool sphereContact(const Vec3f& centre, float radius, Vec3f& normalOut, float& depthOut) const;

private:
    struct Tri {
        Vec3f a, e1, e2, n;  // vertex, edges, unit face normal
        bool twoSided = false;  // the material is drawn without back-face culling: solid from either side
    };
    // One of the game's meshes: its polygons as triangles, under its hierarchy of boxes.
    struct Bvh {
        struct Node {
            Vec3f lo, hi;
            int child[2];  // >= 0: node, < 0: polygon ~value
        };
        std::vector<Tri> tris;
        std::vector<int> polyFirst;  // the triangles of polygon p are tris[polyFirst[p] .. polyFirst[p + 1])
        std::vector<Node> nodes;
        int root = 0;
    };
    std::vector<Tri> tris;
    std::vector<int> large;  // triangles too big for the grid: tested by every query
    std::vector<int> cellStart, cellItems;
    std::vector<Bvh> bvhs;
    Vec3f lo, hi;
    float cell = 6.0f;
    bool bothSidesSolid = false;
    int nx = 0, nz = 0;

    void clear();

    template <class F>
    void forCells(const Vec3f& bmin, const Vec3f& bmax, F&& fn) const;
};

}  // namespace minigame01
