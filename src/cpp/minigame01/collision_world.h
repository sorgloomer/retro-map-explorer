// Static triangle collision for the minigame: a uniform grid over the level's solid triangles.
#pragma once
#include <vector>
#include "../scene.h"

namespace minigame01 {

class CollisionWorld {
public:
    // Takes every opaque / alpha-tested single-sided triangle of the level (glass, water, foliage and glow are not solid).
    // Coordinates are multiplied by `scale` (to work in metres); bothSides: faces count from either side (games that draw double sided).
    void build(const Scene& scene, float scale = 1.0f, bool bothSides = false);
    bool empty() const { return tris.empty(); }

    // First triangle hit by the ray within maxT. Front faces only unless bothSides; returns the unit normal of the hit face
    // (pointing against the ray).
    bool raycast(const Vec3& origin, const Vec3& dir, float maxT, bool bothSides, float& tOut, Vec3& normalOut) const;

    // Deepest overlap of a sphere with the level: normal points from the surface towards the sphere centre.
    bool sphereContact(const Vec3& centre, float radius, Vec3& normalOut, float& depthOut) const;

private:
    struct Tri {
        Vec3 a, e1, e2, n;  // vertex, edges, unit face normal
    };
    std::vector<Tri> tris;
    std::vector<int> large;  // triangles too big for the grid: tested by every query
    std::vector<int> cellStart, cellItems;
    Vec3 lo, hi;
    float cell = 6.0f;
    bool bothSidesSolid = false;
    int nx = 0, nz = 0;

    template <class F>
    void forCells(const Vec3& bmin, const Vec3& bmax, F&& fn) const;
};

}  // namespace minigame01
