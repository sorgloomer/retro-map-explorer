#include "minigame01/box3d_level.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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

void hullTriangles(const b3HullData* hull, std::vector<Vec3f>& vertices, std::vector<uint32_t>& indices) {
    const b3Vec3* points = b3GetHullPoints(hull);
    const b3HullHalfEdge* edges = b3GetHullEdges(hull);
    const b3HullFace* faces = b3GetHullFaces(hull);
    vertices.clear();
    indices.clear();
    for (int i = 0; i < hull->vertexCount; i++) vertices.push_back(Vec3f(points[i].x, points[i].y, points[i].z));
    for (int f = 0; f < hull->faceCount; f++) {
        const int first = faces[f].edge;
        const uint32_t corner0 = edges[first].origin;
        int e = edges[first].next;
        while (edges[e].next != first) {
            indices.insert(indices.end(), {corner0, (uint32_t)edges[e].origin, (uint32_t)edges[edges[e].next].origin});
            e = edges[e].next;
        }
    }
}

bool Box3dLevel::rayCast(const Vec3f& origin, const Vec3f& direction, float maxDistance, float& distance, Vec3f& normal) const {
    if (!created) return false;
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.maskBits = ~(kCarCategory | kPropCategory);
    const b3RayResult hit = b3World_CastRayClosest(worldId, {origin.x, origin.y, origin.z}, {direction.x * maxDistance, direction.y * maxDistance, direction.z * maxDistance}, filter);
    if (!hit.hit) return false;
    distance = hit.fraction * maxDistance;
    normal = Vec3f(hit.normal.x, hit.normal.y, hit.normal.z);
    return true;
}

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
            // The polygons that touch no other polygon (the walls of a fence, a pane of glass) are meant to be solid from both sides: the game draws
            // them two sided, and nothing in its collision files says it (the flags of a polygon are its surface and which of its edges have a
            // neighbour). Such a polygon is recorded a second time the other way round, in a mesh of its own: in the mesh of the level, the two would
            // share all their edges, and the edges of a fence would count as inner ones, which a tyre does not catch on.
            MeshPart back;
            back.vertices = part.vertices;
            back.friction = part.friction;
            back.clockWise = false;   // (the level's meshes are clockwise: this is the other side of the same polygons)
            {
                std::map<std::array<long, 3>, int> weldedId;   // vertices at one place (to a centimetre) are one vertex
                std::vector<int> weld(source.verts.size());
                for (size_t i = 0; i < source.verts.size(); i++) {
                    const Vec3f& v = source.verts[i];
                    const std::array<long, 3> key = {std::lround(v.x * 100.0f), std::lround(v.y * 100.0f), std::lround(v.z * 100.0f)};
                    weld[i] = weldedId.emplace(key, (int)weldedId.size()).first->second;
                }
                std::map<std::pair<int, int>, int> edgeUses;
                auto edgeOf = [&](const uint32_t* corner, int n, int i) {
                    const int a = weld[corner[i]], b = weld[corner[(i + 1) % n]];
                    return std::make_pair(std::min(a, b), std::max(a, b));
                };
                for (const CollisionMesh::Poly& poly : source.polys)
                    for (int i = 0; i < poly.n; i++) edgeUses[edgeOf(&source.corners[poly.first], poly.n, i)]++;
                for (const CollisionMesh::Poly& poly : source.polys) {
                    const uint32_t* corner = &source.corners[poly.first];
                    bool touches = false;
                    for (int i = 0; i < poly.n && !touches; i++) touches = edgeUses[edgeOf(corner, poly.n, i)] > 1;
                    if (touches) continue;
                    for (int k = 2; k < poly.n; k++) {
                        back.indices.insert(back.indices.end(), {(int32_t)corner[0], (int32_t)corner[k - 1], (int32_t)corner[k]});
                        back.surface.push_back(poly.material);
                    }
                }
            }
            parts.push_back(std::move(part));
            if (!back.indices.empty()) parts.push_back(std::move(back));
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
