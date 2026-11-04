// The level for the Box3D physics: the game's own collision meshes (else the level's visible triangles) as static triangle meshes of one
// Box3D world, in metres. The car (Box3dVehicle) lives in the same world.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "../scene/scene.h"

namespace minigame01 {

// The collision categories of what lives in the world of the level besides the level itself (which is in all of them): the rays that look for the
// road (the camera's, the car's ground check) see only the level; the wheels (shape casts) see everything but the car.
constexpr uint64_t kCarCategory = 2;
constexpr uint64_t kPropCategory = 4;

// The faces of a hull (convex polygons, as half-edges) as triangles: every face a fan from its first corner. The points are the hull's, in its own space.
void hullTriangles(const b3HullData* hull, std::vector<Vec3f>& vertices, std::vector<uint32_t>& indices);

class Box3dLevel {
public:
    Box3dLevel() = default;
    Box3dLevel(const Box3dLevel&) = delete;
    Box3dLevel& operator=(const Box3dLevel&) = delete;
    ~Box3dLevel() { destroy(); }

    // `unitsPerMetre`: level units per metre (the meshes are scaled down to metres). `bothSides`: the faces of the visible triangles
    // count from either side. False when the level has no triangle to stand on.
    bool build(const Scene& scene, float unitsPerMetre, bool bothSides);
    void destroy();
    bool valid() const { return created; }
    b3WorldId world() const { return worldId; }
    b3BodyId ground() const { return groundId; }
    // A ray cast against the level alone (not the car, nor the movable objects): from `origin` along the unit vector `direction`, in metres. The
    // distance to the first surface that faces the ray and that surface's normal. Meshes are solid from their front side only.
    bool rayCast(const Vec3f& origin, const Vec3f& direction, float maxDistance, float& distance, Vec3f& normal) const;
    // The multiplier of the tyre friction of a surface, from the user material id that a ray cast reports for the triangle it hits.
    float surfaceFriction(uint64_t userMaterialId) const;

private:
    bool created = false;
    b3WorldId worldId = b3_nullWorldId;
    b3BodyId groundId = b3_nullBodyId;
    std::vector<b3MeshData*> meshes;                         // the shapes refer to them: they live as long as the world
    std::vector<std::vector<b3SurfaceMaterial>> materials;   // per mesh: tyre friction by surface type
    std::vector<std::vector<float>> frictionBySurface;       // the same numbers, per mesh (the user material id says which)
};

}  // namespace minigame01
