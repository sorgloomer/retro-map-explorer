#ifndef RME_PHYSICS_LEGACY  // (the build says --legacy-physics: the car runs on vehicle.cpp, and Box3D is not part of the build)
#include "minigame01/box3d_level.h"
#include <algorithm>
#include <cstdint>
#include "minigame01/params.h"

namespace minigame01 {

namespace {

// The friction of two surfaces together: the product of their coefficients (the tyre's grip times the multiplier of the surface the game
// says: grass 0.8, road 1, ...), not the geometric mean that Box3D does by default.
float frictionProduct(float frictionA, uint64_t, float frictionB, uint64_t) { return frictionA * frictionB; }

// One mesh of the level: triangles in metres with a surface type each.
struct MeshPart {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
    std::vector<uint8_t> surface;   // per triangle
    std::vector<float> friction;    // by surface type
    bool clockWise = false;         // the winding of the triangles, seen from the side that is solid
};

}  // namespace

float Box3dLevel::surfaceFriction(uint64_t userMaterialId) const {
    const size_t mesh = (size_t)(userMaterialId >> 16), surface = (size_t)(userMaterialId & 0xffff);
    if (mesh >= frictionBySurface.size() || surface >= frictionBySurface[mesh].size()) return 1.0f;
    return frictionBySurface[mesh][surface];
}

void Box3dLevel::destroy() {
    if (created) b3DestroyWorld(worldId);  // (its shapes are gone: the meshes can go)
    for (b3MeshData* mesh : meshes) b3DestroyMesh(mesh);
    meshes.clear();
    materials.clear();
    frictionBySurface.clear();
    worldId = b3_nullWorldId;
    groundId = b3_nullBodyId;
    created = false;
}

bool Box3dLevel::build(const Scene& scene, float unitsPerMetre, bool bothSides) {
    destroy();
    // The tolerances of the engine are made for a car of a few metres: a toy car is a tenth of that, and so are its tolerances (this is a
    // setting of the whole engine, so it follows the car that is driven; a car is chosen when the level is loaded).
    b3SetLengthUnitsPerMeter(std::max(0.05f, carSettings().sizeScale));
    const float toMetres = 1.0f / unitsPerMetre;
    std::vector<MeshPart> parts;
    if (!scene.collisionMeshes.empty()) {
        for (const CollisionMesh& source : scene.collisionMeshes) {
            MeshPart part;
            part.clockWise = true;  // the winding of the game's collision meshes (Carmageddon's): the other way round to Box3D's
            part.vertices.reserve(source.verts.size());
            for (const Vec3f& v : source.verts) part.vertices.push_back({v.x * toMetres, v.y * toMetres, v.z * toMetres});
            uint8_t highestSurface = 0;
            for (const CollisionMesh::Poly& poly : source.polys) {
                const uint32_t* corner = &source.corners[poly.first];
                for (int k = 2; k < poly.n; k++) {  // a fan: (0 1 2), (0 2 3), ...
                    part.indices.insert(part.indices.end(), {(int32_t)corner[0], (int32_t)corner[k - 1], (int32_t)corner[k]});
                    part.surface.push_back(poly.material);
                }
                highestSurface = std::max(highestSurface, poly.material);
            }
            for (int s = 0; s <= highestSurface; s++) part.friction.push_back(source.frictionOf((uint8_t)s));
            parts.push_back(std::move(part));
        }
    } else {
        // no collision files: the level as it is drawn (one surface type, plain road)
        for (const Batch& batch : scene.batches) {
            MeshPart part;
            part.vertices.reserve(batch.verts.size());
            for (const SceneVertex& v : batch.verts) part.vertices.push_back({v.x * toMetres, v.y * toMetres, v.z * toMetres});
            for (size_t i = 0; i + 2 < batch.idx.size(); i += 3) {
                part.indices.insert(part.indices.end(), {(int32_t)batch.idx[i], (int32_t)batch.idx[i + 1], (int32_t)batch.idx[i + 2]});
                part.surface.push_back(0);
            }
            part.friction.push_back(1.0f);
            parts.push_back(std::move(part));
        }
    }
    (void)bothSides;  // (a triangle of a Box3D mesh is solid from one side only, the one its winding says: every triangle is recorded once)

    b3WorldDef worldDef = b3DefaultWorldDef();
    worldDef.frictionCallback = frictionProduct;
    worldDef.enableSleep = false;  // the car is always in play
    worldDef.workerCount = 1;      // one thread: no task system
    worldId = b3CreateWorld(&worldDef);
    b3BodyDef groundDef = b3DefaultBodyDef();
    groundId = b3CreateBody(worldId, &groundDef);
    created = true;

    bool any = false;
    for (MeshPart& part : parts) {
        if (part.vertices.size() < 3 || part.indices.empty()) continue;
        b3MeshDef def = {};
        def.vertices = part.vertices.data();
        def.stride = 0;
        def.vertexCount = (int)part.vertices.size();
        def.indices = part.indices.data();
        def.triangleCount = (int)(part.indices.size() / 3);
        def.clockWiseWinding = part.clockWise;
        def.materialIndices = part.surface.data();
        def.identifyEdges = true;  // the edges that two triangles share are inner ones: a tyre rolling over them does not catch on them
        b3MeshData* mesh = b3CreateMesh(&def, nullptr, 0);
        if (!mesh) continue;
        meshes.push_back(mesh);
        materials.emplace_back();
        frictionBySurface.push_back(part.friction);
        for (size_t surface = 0; surface < part.friction.size(); surface++) {
            b3SurfaceMaterial material = b3DefaultSurfaceMaterial();
            material.friction = part.friction[surface];
            material.userMaterialId = ((uint64_t)(meshes.size() - 1) << 16) | (uint64_t)surface;   // which mesh, which surface type
            materials.back().push_back(material);
        }
        b3ShapeDef shapeDef = b3DefaultShapeDef();
        shapeDef.materials = materials.back().data();
        shapeDef.materialCount = (int)materials.back().size();
        b3CreateMeshShape(groundId, &shapeDef, mesh, {1.0f, 1.0f, 1.0f});
        any = true;
    }
    if (!any) destroy();
    return any;
}

}  // namespace minigame01
#endif  // RME_PHYSICS_LEGACY
