#include "minigame01/box3d_props.h"
#include "minigame01/box3d_level.h"
#include <algorithm>

namespace minigame01 {

namespace {

constexpr int kMaxHullVertices = 64;   // of the collider of a kind of object (the visible mesh of one can have thousands of points)
constexpr float kFriction = 0.8f;      // of the objects (against the level: times the multiplier of the surface; against each other: the product)
constexpr float kRestitution = 0.1f;
constexpr float kThinnest = 0.15f;      // m: a collider thinner than this (a fence, a pane of glass: flat meshes) is made a box this thick
constexpr float kDefaultDensity = 150.0f;   // kg/m^3: what an object has that the game's files give no mass for (its volume decides)
constexpr float kMinMass = 20.0f, kMaxMass = 5000.0f;

// The pose of a body from the model matrix of its object (level units), in metres.
void poseOf(const Mat4f& placement, float toMetres, b3Vec3& position, b3Quat& rotation) {
    position = {placement.m[12] * toMetres, placement.m[13] * toMetres, placement.m[14] * toMetres};
    b3Matrix3 m;
    m.cx = {placement.m[0], placement.m[1], placement.m[2]};
    m.cy = {placement.m[4], placement.m[5], placement.m[6]};
    m.cz = {placement.m[8], placement.m[9], placement.m[10]};
    rotation = b3MakeQuatFromMatrix(&m);
}

// The model matrix (level units) of a body, from its pose in metres.
Mat4f modelOf(const b3WorldTransform& pose, float unitsPerMetre) {
    const b3Matrix3 m = b3MakeMatrixFromQuat(pose.q);
    Mat4f model = Mat4f::identity();
    model.m[0] = m.cx.x; model.m[1] = m.cx.y; model.m[2] = m.cx.z;
    model.m[4] = m.cy.x; model.m[5] = m.cy.y; model.m[6] = m.cy.z;
    model.m[8] = m.cz.x; model.m[9] = m.cz.y; model.m[10] = m.cz.z;
    model.m[12] = pose.p.x * unitsPerMetre;
    model.m[13] = pose.p.y * unitsPerMetre;
    model.m[14] = pose.p.z * unitsPerMetre;
    return model;
}

// A hull as a solid, flat shaded shape (every triangle has its own vertices and the normal of its face, away from the middle of the hull),
// in level units, for the lit shader.
BatchSet solidShape(const std::vector<Vec3f>& points, const std::vector<uint32_t>& indices, float toUnits, uint8_t r, uint8_t g, uint8_t b) {
    Vec3f middle;
    for (const Vec3f& p : points) middle += p;
    if (!points.empty()) middle = middle * (1.0f / (float)points.size());
    std::vector<SceneVertex> vertices;
    std::vector<Vec3f> normals;
    std::vector<uint32_t> triangles;
    for (size_t k = 0; k + 2 < indices.size(); k += 3) {
        const Vec3f& p0 = points[indices[k]];
        const Vec3f& p1 = points[indices[k + 1]];
        const Vec3f& p2 = points[indices[k + 2]];
        Vec3f normal = cross(p1 - p0, p2 - p0);
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length > 1e-12f) normal = normal * (1.0f / length);
        const Vec3f outward = (p0 + p1 + p2) * (1.0f / 3.0f) - middle;
        if (dot(normal, outward) < 0) normal = normal * -1.0f;
        for (const Vec3f* p : {&p0, &p1, &p2}) {
            SceneVertex v;
            v.x = p->x * toUnits; v.y = p->y * toUnits; v.z = p->z * toUnits;
            v.r = r; v.g = g; v.b = b; v.a = 255;
            v.u = v.v = 0;
            triangles.push_back((uint32_t)vertices.size());
            vertices.push_back(v);
            normals.push_back(normal);
        }
    }
    BatchSet shape;
    shape.lit = true;
    shape.appendIndexed(-1, BLEND_OPAQUE, vertices.data(), vertices.size(), triangles.data(), triangles.size(), true, normals.data());
    return shape;
}

}  // namespace

void Box3dProps::build(Scene& scene, b3WorldId world, float unitsPerMetre) {
    destroy();
    unitsPerMetre_ = unitsPerMetre;
    const float toMetres = 1.0f / unitsPerMetre;
    // the collider of every kind, made once: the convex hull of its points (a box round them when they make none, or when the object is
    // flat), and what the body is made of
    std::vector<b3HullData*> hulls(scene.propTypes.size(), nullptr);
    std::vector<float> densities(scene.propTypes.size(), 1.0f);
    std::vector<b3MassData> massData(scene.propTypes.size());
    for (size_t t = 0; t < scene.propTypes.size(); t++) {
        const Scene::PropType type = scene.propTypes[t];   // (a copy: the scene's list grows below)
        std::vector<b3Vec3> points;
        points.reserve(type.collider.size());
        b3Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (const Vec3f& p : type.collider) {
            const b3Vec3 q = {p.x * toMetres, p.y * toMetres, p.z * toMetres};
            points.push_back(q);
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        const bool flat = !points.empty() && std::min(hi.x - lo.x, std::min(hi.y - lo.y, hi.z - lo.z)) < kThinnest;
        if (points.size() >= 4 && !flat) hulls[t] = b3CreateHull(points.data(), (int)points.size(), std::min<int>(kMaxHullVertices, (int)points.size()));
        if (!hulls[t] && !points.empty()) {
            if (hi.x - lo.x < kThinnest) { const float c = 0.5f * (lo.x + hi.x); lo.x = c - 0.5f * kThinnest; hi.x = c + 0.5f * kThinnest; }
            if (hi.y - lo.y < kThinnest) { const float c = 0.5f * (lo.y + hi.y); lo.y = c - 0.5f * kThinnest; hi.y = c + 0.5f * kThinnest; }
            if (hi.z - lo.z < kThinnest) { const float c = 0.5f * (lo.z + hi.z); lo.z = c - 0.5f * kThinnest; hi.z = c + 0.5f * kThinnest; }
            const b3Vec3 corners[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
                                       {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}};
            hulls[t] = b3CreateHull(corners, 8, 8);
        }
        if (hulls[t]) {
            // the collider as a solid shape, for the display (in body space, level units)
            std::vector<Vec3f> shapePoints;
            std::vector<uint32_t> shapeIndices;
            hullTriangles(hulls[t], shapePoints, shapeIndices);
            for (int state = 0; state < 2; state++) {
                scene.moverGeoms.push_back(state == 0 ? solidShape(shapePoints, shapeIndices, unitsPerMetre, 90, 160, 255) : solidShape(shapePoints, shapeIndices, unitsPerMetre, 255, 160, 60));
                scene.propTypes[t].colliderGeom[state] = (int)scene.moverGeoms.size() - 1;
            }
            const float volume = std::max(b3ComputeHullMass(hulls[t], 1.0f).mass, 1e-4f);   // (the mass at a density of 1)
            const float mass = type.mass > 0 ? type.mass : std::min(kMaxMass, std::max(kMinMass, volume * kDefaultDensity));
            densities[t] = mass / volume;
            massData[t] = b3ComputeHullMass(hulls[t], densities[t]);
        }
    }
    bodies_.assign(scene.props.size(), Body());
    for (size_t i = 0; i < scene.props.size(); i++) {
        const Scene::Prop& prop = scene.props[i];
        if (prop.type < 0 || prop.type >= (int)hulls.size() || !hulls[prop.type]) continue;
        b3BodyDef def = b3DefaultBodyDef();
        def.type = b3_dynamicBody;
        poseOf(prop.placement, toMetres, def.position, def.rotation);
        def.isAwake = false;   // asleep until something touches it
        const b3BodyId body = b3CreateBody(world, &def);
        b3ShapeDef shape = b3DefaultShapeDef();
        shape.density = densities[prop.type];
        shape.baseMaterial.friction = kFriction;
        shape.baseMaterial.restitution = kRestitution;
        shape.filter.categoryBits = kPropCategory;
        b3CreateHullShape(body, &shape, hulls[prop.type]);
        // The body's origin is the centre of mass that the game's files say (or the middle of the mesh): the mass is where the files put it, not
        // at the centroid of the hull. The inertia of the hull about its centroid, moved there (the parallel axis theorem).
        {
            const b3MassData& hullMass = massData[prop.type];
            const b3Vec3 d = hullMass.center;   // from the new centre of mass to the centroid
            const float dd = d.x * d.x + d.y * d.y + d.z * d.z, m = hullMass.mass;
            b3MassData data;
            data.mass = m;
            data.center = {0, 0, 0};
            data.inertia = hullMass.inertia;
            data.inertia.cx.x += m * (dd - d.x * d.x); data.inertia.cx.y -= m * d.x * d.y; data.inertia.cx.z -= m * d.x * d.z;
            data.inertia.cy.x -= m * d.y * d.x; data.inertia.cy.y += m * (dd - d.y * d.y); data.inertia.cy.z -= m * d.y * d.z;
            data.inertia.cz.x -= m * d.z * d.x; data.inertia.cz.y -= m * d.z * d.y; data.inertia.cz.z += m * (dd - d.z * d.z);
            b3Body_SetMassData(body, data);
        }
        bodies_[i].id = body;
    }
    showColliders_ = false;
    for (b3HullData* hull : hulls)
        if (hull) b3DestroyHull(hull);
}

void Box3dProps::reset(Scene& scene) {
    const float toMetres = 1.0f / unitsPerMetre_;
    for (size_t i = 0; i < bodies_.size() && i < scene.props.size(); i++) {
        const Scene::Prop& prop = scene.props[i];
        if (prop.mover >= 0 && prop.mover < (int)scene.movers.size()) scene.movers[prop.mover].model = prop.placement;
        if (B3_IS_NULL(bodies_[i].id)) continue;
        b3Vec3 position;
        b3Quat rotation;
        poseOf(prop.placement, toMetres, position, rotation);
        b3Body_SetTransform(bodies_[i].id, position, rotation);
        b3Body_SetLinearVelocity(bodies_[i].id, {0, 0, 0});
        b3Body_SetAngularVelocity(bodies_[i].id, {0, 0, 0});
        b3Body_SetAwake(bodies_[i].id, false);
        bodies_[i].wasAwake = false;
        if (showColliders_) show(scene, i, false);
    }
}

void Box3dProps::sync(Scene& scene) {
    for (size_t i = 0; i < bodies_.size() && i < scene.props.size(); i++) {
        Body& body = bodies_[i];
        if (B3_IS_NULL(body.id)) continue;
        const bool awake = b3Body_IsAwake(body.id);
        if (!awake && !body.wasAwake) continue;   // (one more update for a body that fell asleep: it has stopped where it is)
        body.wasAwake = awake;
        const int mover = scene.props[i].mover;
        if (mover >= 0 && mover < (int)scene.movers.size()) scene.movers[mover].model = modelOf(b3Body_GetTransform(body.id), unitsPerMetre_);
        if (showColliders_) show(scene, i, awake);
    }
}

void Box3dProps::show(Scene& scene, size_t index, bool awake) const {
    const Scene::Prop& prop = scene.props[index];
    if (prop.mover < 0 || prop.mover >= (int)scene.movers.size()) return;
    const Scene::PropType& type = scene.propTypes[prop.type];
    Mover& mover = scene.movers[prop.mover];
    const int solid = type.colliderGeom[awake ? 1 : 0];
    if (showColliders_ && solid >= 0) {
        mover.geom = solid;
        mover.lit = true;
    } else {
        mover.geom = type.geom;
        mover.lit = false;
    }
}

void Box3dProps::showColliders(Scene& scene, bool on) {
    showColliders_ = on;
    for (size_t i = 0; i < scene.props.size(); i++)
        show(scene, i, i < bodies_.size() && !B3_IS_NULL(bodies_[i].id) && b3Body_IsAwake(bodies_[i].id));
}

}  // namespace minigame01
