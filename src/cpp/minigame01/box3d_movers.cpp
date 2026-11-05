#include "minigame01/box3d_movers.h"
#include <algorithm>
#include <cmath>
#include "minigame01/box3d_level.h"
#include "minigame01/collider_shapes.h"
#include "minigame01/convex_decomposition.h"
#include "scene/mover_pose.h"

namespace minigame01 {

namespace {

constexpr float kFriction = 0.8f;
constexpr float kThinnest = 0.15f;      // m: a piece of a collider thinner than this is made this thick (it has no hull otherwise)
constexpr size_t kMaxPieces = 48;       // the most convex pieces that a mesh is made into (more: one box round all of it)

// The pose of a body from the model matrix of its object (level units), in metres.
b3WorldTransform poseOf(const Mat4f& placement, float toMetres) {
    b3WorldTransform pose;
    pose.p = {placement.m[12] * toMetres, placement.m[13] * toMetres, placement.m[14] * toMetres};
    b3Matrix3 m;
    m.cx = {placement.m[0], placement.m[1], placement.m[2]};
    m.cy = {placement.m[4], placement.m[5], placement.m[6]};
    m.cz = {placement.m[8], placement.m[9], placement.m[10]};
    pose.q = b3MakeQuatFromMatrix(&m);
    return pose;
}

// The box of some points made at least kThinnest thick: the corners.
std::vector<Vec3f> thickBox(const std::vector<Vec3f>& points) {
    Vec3f lo = points[0], hi = points[0];
    for (const Vec3f& p : points) {
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    float* l = &lo.x;
    float* h = &hi.x;
    for (int k = 0; k < 3; k++)
        if (h[k] - l[k] < kThinnest) { const float c = 0.5f * (l[k] + h[k]); l[k] = c - 0.5f * kThinnest; h[k] = c + 0.5f * kThinnest; }
    std::vector<Vec3f> corners;
    for (int c = 0; c < 8; c++) corners.push_back(Vec3f((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z));
    return corners;
}

// A sphere as a piece of a shape for the display.
void sphereShape(Vec3f centre, float radius, ColliderPiece& piece) {
    constexpr int kSegments = 16, kRings = 10;
    const float pi = 3.14159265f;
    piece.points.push_back(centre + Vec3f(0, radius, 0));
    for (int ring = 1; ring < kRings; ring++) {
        const float polar = pi * (float)ring / (float)kRings;
        for (int segment = 0; segment < kSegments; segment++) {
            const float azimuth = 2 * pi * (float)segment / (float)kSegments;
            piece.points.push_back(centre + Vec3f(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth)) * radius);
        }
    }
    piece.points.push_back(centre - Vec3f(0, radius, 0));
    const uint32_t bottom = (uint32_t)piece.points.size() - 1;
    auto at = [&](int ring, int segment) { return (uint32_t)(1 + (ring - 1) * kSegments + (segment % kSegments)); };
    for (int segment = 0; segment < kSegments; segment++) {
        piece.indices.insert(piece.indices.end(), {0u, at(1, segment + 1), at(1, segment)});
        piece.indices.insert(piece.indices.end(), {bottom, at(kRings - 1, segment), at(kRings - 1, segment + 1)});
        for (int ring = 1; ring + 1 < kRings; ring++) {
            piece.indices.insert(piece.indices.end(), {at(ring, segment), at(ring, segment + 1), at(ring + 1, segment)});
            piece.indices.insert(piece.indices.end(), {at(ring, segment + 1), at(ring + 1, segment + 1), at(ring + 1, segment)});
        }
    }
}

}  // namespace

void Box3dMovers::build(Scene& scene, b3WorldId world, float unitsPerMetre, double time) {
    destroy();
    lastTime_ = -1;
    unitsPerMetre_ = unitsPerMetre;
    const float toMetres = 1.0f / unitsPerMetre;
    const size_t moverCount = scene.movers.size();   // (the scene's list of geometries grows below)
    for (size_t i = 0; i < moverCount; i++) {
        const Mover& m = scene.movers[i];
        if (m.fixed || m.billboard || m.external || m.staticModel || m.powerupMesh || m.geom < 0 || m.geom >= (int)scene.moverGeoms.size()) continue;
        Mat4f start;
        if (!moverPoseAt(m, (float)std::fmod(time, 100000.0), start)) continue;   // (not one that moves)
        // the solid parts, in its own space, in metres: what its collision files say, else its mesh
        std::vector<Vec3f> points;
        std::vector<uint32_t> triangles;
        std::vector<std::pair<Vec3f, float>> spheres;
        const bool fromFile = m.collider.points.size() >= 4 || !m.collider.spheres.empty();
        if (fromFile) {
            for (const Vec3f& p : m.collider.points) points.push_back(p * toMetres);
            triangles = m.collider.triangles;
            for (const Mover::Collider::Sphere& s : m.collider.spheres) spheres.emplace_back(s.centre * toMetres, s.radius * toMetres);
        } else {
            for (const Batch& batch : scene.moverGeoms[m.geom].batches) {
                if (batch.blend == BLEND_ADD) continue;   // (glow, not a thing)
                const uint32_t first = (uint32_t)points.size();
                for (const SceneVertex& v : batch.verts) points.push_back(Vec3f(v.x * toMetres, v.y * toMetres, v.z * toMetres));
                for (uint32_t index : batch.idx) triangles.push_back(first + index);
            }
        }
        if (points.size() < 3 && spheres.empty()) continue;
        std::vector<std::vector<Vec3f>> pieces;
        if (points.size() >= 3) {
            pieces = decomposeIntoConvexPieces(points, triangles);
            if (pieces.empty() || (!fromFile && pieces.size() > kMaxPieces)) { pieces.clear(); pieces.push_back(points); }
        }
        b3BodyDef def = b3DefaultBodyDef();
        def.type = b3_kinematicBody;
        const b3WorldTransform pose = poseOf(start, toMetres);
        def.position = pose.p;
        def.rotation = pose.q;
        const b3BodyId body = b3CreateBody(world, &def);
        b3ShapeDef shape = b3DefaultShapeDef();
        shape.baseMaterial.friction = kFriction;
        shape.filter.categoryBits = kMoverCategory;
        std::vector<ColliderPiece> shapes;   // the pieces as they are drawn, in metres
        for (std::vector<Vec3f>& piece : pieces) {
            std::vector<b3Vec3> hullPoints;
            for (const Vec3f& p : piece) hullPoints.push_back({p.x, p.y, p.z});
            b3HullData* hull = hullPoints.size() >= 4 ? b3CreateHull(hullPoints.data(), (int)hullPoints.size(), 64) : nullptr;
            if (!hull || b3ComputeHullMass(hull, 1.0f).mass < 1e-6f) {   // flat or too small: a box round it
                if (hull) b3DestroyHull(hull);
                const std::vector<Vec3f> box = thickBox(piece);
                std::vector<b3Vec3> corners;
                for (const Vec3f& p : box) corners.push_back({p.x, p.y, p.z});
                hull = b3CreateHull(corners.data(), (int)corners.size(), 8);
            }
            if (!hull) continue;
            b3CreateHullShape(body, &shape, hull);
            shapes.emplace_back();
            hullTriangles(hull, shapes.back().points, shapes.back().indices);
            b3DestroyHull(hull);   // (the shape has its own copy)
        }
        for (const auto& s : spheres) {
            const b3Sphere sphere = {{s.first.x, s.first.y, s.first.z}, s.second};
            b3CreateSphereShape(body, &shape, &sphere);
            shapes.emplace_back();
            sphereShape(s.first, s.second, shapes.back());
        }
        if (shapes.empty()) { b3DestroyBody(body); continue; }
        Body entry;
        entry.id = body;
        entry.mover = (int)i;
        entry.meshGeom = m.geom;
        // the collider as solid shapes, for the display (in the space of the geometry, level units)
        scene.moverGeoms.push_back(solidColliderShape(shapes, unitsPerMetre, 90, 160, 255, (uint32_t)(i + 3000)));
        entry.colliderGeom = (int)scene.moverGeoms.size() - 1;
        if (fromFile && m.collider.triangles.size() >= 3) {   // the polyhedra as the files have them, and the spheres
            std::vector<ColliderPiece> raw;
            for (SurfaceComponent& component : splitSurfaceIntoComponents(m.collider.points, m.collider.triangles)) {
                ColliderPiece piece;
                piece.points = std::move(component.points);
                piece.indices = std::move(component.triangles);
                piece.closed = true;
                raw.push_back(std::move(piece));
            }
            for (const Mover::Collider::Sphere& s : m.collider.spheres) {
                raw.emplace_back();
                sphereShape(s.centre, s.radius, raw.back());
            }
            scene.moverGeoms.push_back(solidColliderShape(raw, 1.0f, 90, 160, 255, (uint32_t)(i + 3000 + 7919)));
            entry.rawGeom = (int)scene.moverGeoms.size() - 1;
        }
        bodies_.push_back(entry);
    }
}

void Box3dMovers::update(const Scene& scene, double time, float seconds) {
    const float toMetres = 1.0f / unitsPerMetre_;
    if (lastTime_ < 0 || std::fabs(time - lastTime_) > 0.5) {   // the first time, or the clock was set: they go where it says, not at a speed
        for (const Body& body : bodies_) {
            Mat4f model;
            if (moverPoseAt(scene.movers[body.mover], (float)std::fmod(time, 100000.0), model)) {
                const b3WorldTransform pose = poseOf(model, toMetres);
                b3Body_SetTransform(body.id, pose.p, pose.q);
            }
        }
    }
    lastTime_ = time;
    if (seconds <= 0) return;
    const float at = (float)std::fmod(time + seconds, 100000.0);
    for (const Body& body : bodies_) {
        Mat4f model;
        if (!moverPoseAt(scene.movers[body.mover], at, model)) continue;
        b3Body_SetTargetTransform(body.id, poseOf(model, toMetres), seconds, true);
    }
}

void Box3dMovers::showColliders(Scene& scene, int mode) {
    for (const Body& body : bodies_) {
        Mover& mover = scene.movers[body.mover];
        const int solid = mode == 2 && body.rawGeom >= 0 ? body.rawGeom : body.colliderGeom;
        if (mode != 0 && solid >= 0) {
            mover.geom = solid;
            mover.lit = true;
        } else {
            mover.geom = body.meshGeom;
            mover.lit = false;
        }
    }
}

}  // namespace minigame01
