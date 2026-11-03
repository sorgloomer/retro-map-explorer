// The level for the Box3D physics: the game's own collision meshes (else the level's visible triangles) as static triangle meshes of one
// Box3D world, in metres. The car (Box3dVehicle) lives in the same world.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "../scene/scene.h"

namespace minigame01 {

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
