#include "minigame01/box3d_props.h"
#include "minigame01/box3d_level.h"
#include "minigame01/collider_shapes.h"
#include "minigame01/convex_decomposition.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

constexpr int kMaxHullVertices = 64;   // of the collider of a kind of object (the visible mesh of one can have thousands of points)
constexpr float kFriction = 0.8f;      // of the objects (against the level: times the multiplier of the surface; against each other: the product)
constexpr float kRestitution = 0.1f;
constexpr float kThinnest = 0.15f;      // m: a piece of a collider thinner than this (a fence, a pane of glass: flat meshes, which can lie any way) is made this thick
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

// Gives a piece the thickness of at least kThinnest in every direction (a pane of glass has none, and no hull can be made of it): along the
// axes of the points (the eigenvectors of their covariance), the points are copied to either side of what is thinner than that.
void thicken(std::vector<Vec3f>& points) {
    if (points.size() < 3) return;
    Vec3f centroid;
    for (const Vec3f& p : points) centroid += p;
    centroid = centroid * (1.0f / (float)points.size());
    double c[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const Vec3f& p : points) {
        const double d[3] = {p.x - centroid.x, p.y - centroid.y, p.z - centroid.z};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) c[i][j] += d[i] * d[j];
    }
    // Jacobi rotations: the symmetric matrix becomes diagonal, the columns of v are its eigenvectors
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 12; sweep++) {
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                if (std::fabs(c[p][q]) < 1e-18) continue;
                const double theta = 0.5 * std::atan2(2.0 * c[p][q], c[q][q] - c[p][p]);
                const double cs = std::cos(theta), sn = std::sin(theta);
                for (int k = 0; k < 3; k++) {   // c = J^T c J
                    const double kp = c[k][p], kq = c[k][q];
                    c[k][p] = cs * kp - sn * kq;
                    c[k][q] = sn * kp + cs * kq;
                }
                for (int k = 0; k < 3; k++) {
                    const double pk = c[p][k], qk = c[q][k];
                    c[p][k] = cs * pk - sn * qk;
                    c[q][k] = sn * pk + cs * qk;
                }
                for (int k = 0; k < 3; k++) {
                    const double kp = v[k][p], kq = v[k][q];
                    v[k][p] = cs * kp - sn * kq;
                    v[k][q] = sn * kp + cs * kq;
                }
            }
    }
    for (int axis = 0; axis < 3; axis++) {
        const Vec3f direction((float)v[0][axis], (float)v[1][axis], (float)v[2][axis]);
        float low = 1e30f, high = -1e30f;
        for (const Vec3f& p : points) {
            const float along = dot(direction, p - centroid);
            low = std::min(low, along);
            high = std::max(high, along);
        }
        if (high - low >= kThinnest) continue;
        const float extra = 0.5f * (kThinnest - (high - low));
        const size_t count = points.size();
        points.reserve(2 * count);
        for (size_t i = 0; i < count; i++) {
            const Vec3f p = points[i];
            points[i] = p + direction * extra;
            points.push_back(p - direction * extra);
        }
    }
}

}  // namespace

void Box3dProps::build(Scene& scene, b3WorldId world, float unitsPerMetre) {
    destroy();
    unitsPerMetre_ = unitsPerMetre;
    const float toMetres = 1.0f / unitsPerMetre;
    // The collider of every kind, made once: the convex pieces of its surface (one for an object that is convex), a box round it when it is
    // flat or the pieces cannot be made; the mass data of the whole.
    std::vector<std::vector<b3HullData*>> hulls(scene.propTypes.size());
    std::vector<float> densities(scene.propTypes.size(), 1.0f);
    std::vector<b3MassData> massData(scene.propTypes.size());
    for (size_t t = 0; t < scene.propTypes.size(); t++) {
        const Scene::PropType type = scene.propTypes[t];   // (a copy: the scene's list grows below)
        std::vector<b3Vec3> points;
        std::vector<Vec3f> metrePoints;
        points.reserve(type.collider.size());
        b3Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (const Vec3f& p : type.collider) {
            const b3Vec3 q = {p.x * toMetres, p.y * toMetres, p.z * toMetres};
            points.push_back(q);
            metrePoints.push_back(Vec3f(q.x, q.y, q.z));
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        std::vector<b3HullData*>& pieces = hulls[t];
        if (points.size() >= 3) {
            // the surface in convex pieces; an object that has no surface to split makes one hull of all its points
            std::vector<std::vector<Vec3f>> parts;
            if (type.colliderTriangles.size() >= 3) {
                // The object as several polyhedra (the ones that share an edge are one), each in convex pieces
                parts = decomposeIntoConvexPieces(metrePoints, type.colliderTriangles);
            }
            if (parts.empty()) parts.push_back(metrePoints);
            for (std::vector<Vec3f>& part : parts) {
                thicken(part);   // (a piece of zero thickness has no hull)
                std::vector<b3Vec3> partPoints;
                for (const Vec3f& p : part) partPoints.push_back({p.x, p.y, p.z});
                b3HullData* hull = partPoints.size() >= 4 ? b3CreateHull(partPoints.data(), (int)partPoints.size(), kMaxHullVertices) : nullptr;
                if (hull && b3ComputeHullMass(hull, 1.0f).mass > 1e-6f) pieces.push_back(hull);
                else if (hull) b3DestroyHull(hull);
            }
        }
        if (pieces.empty() && !points.empty()) {
            if (hi.x - lo.x < kThinnest) { const float c = 0.5f * (lo.x + hi.x); lo.x = c - 0.5f * kThinnest; hi.x = c + 0.5f * kThinnest; }
            if (hi.y - lo.y < kThinnest) { const float c = 0.5f * (lo.y + hi.y); lo.y = c - 0.5f * kThinnest; hi.y = c + 0.5f * kThinnest; }
            if (hi.z - lo.z < kThinnest) { const float c = 0.5f * (lo.z + hi.z); lo.z = c - 0.5f * kThinnest; hi.z = c + 0.5f * kThinnest; }
            const b3Vec3 corners[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
                                       {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}};
            b3HullData* hull = b3CreateHull(corners, 8, 8);
            if (hull) pieces.push_back(hull);
        }
        if (pieces.empty()) continue;
        // the collider as solid shapes, for the display (in body space, level units)
        std::vector<ColliderPiece> shapes(pieces.size());
        for (size_t p = 0; p < pieces.size(); p++) hullTriangles(pieces[p], shapes[p].points, shapes[p].indices);
        for (int state = 0; state < 2; state++) {
            scene.moverGeoms.push_back(state == 0 ? solidColliderShape(shapes, unitsPerMetre, 90, 160, 255, (uint32_t)t) : solidColliderShape(shapes, unitsPerMetre, 255, 160, 60, (uint32_t)t));
            scene.propTypes[t].colliderGeom[state] = (int)scene.moverGeoms.size() - 1;
        }
        // the shape as the game's file has it: its polyhedra as they are (concave ones too), in body space, level units already
        if (type.colliderTriangles.size() >= 3) {
            std::vector<ColliderPiece> raw;
            for (SurfaceComponent& component : splitSurfaceIntoComponents(type.collider, type.colliderTriangles)) {
                ColliderPiece piece;
                piece.points = std::move(component.points);
                piece.indices = std::move(component.triangles);
                piece.closed = type.colliderFromFile;
                raw.push_back(std::move(piece));
            }
            for (int state = 0; state < 2; state++) {
                scene.moverGeoms.push_back(state == 0 ? solidColliderShape(raw, 1.0f, 90, 160, 255, (uint32_t)t + 7919) : solidColliderShape(raw, 1.0f, 255, 160, 60, (uint32_t)t + 7919));
                scene.propTypes[t].rawGeom[state] = (int)scene.moverGeoms.size() - 1;
            }
        }
        // the mass: the files', else from the volume of the pieces (which are all of one density); the inertia is that of the pieces about the
        // origin of the body, which is the centre of mass that the files say (or the middle of the mesh), not the centroid of the pieces
        float volume = 0;
        for (b3HullData* hull : pieces) volume += b3ComputeHullMass(hull, 1.0f).mass;   // (the mass at a density of 1)
        volume = std::max(volume, 1e-4f);
        const float mass = type.mass > 0 ? type.mass : std::min(kMaxMass, std::max(kMinMass, volume * kDefaultDensity));
        densities[t] = mass / volume;
        b3MassData total;
        total.mass = 0;
        total.center = {0, 0, 0};
        total.inertia = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        for (b3HullData* hull : pieces) {
            const b3MassData piece = b3ComputeHullMass(hull, densities[t]);
            const b3Vec3 d = piece.center;   // from the origin to the centroid of the piece (the parallel axis theorem)
            const float dd = d.x * d.x + d.y * d.y + d.z * d.z, m = piece.mass;
            total.mass += m;
            total.inertia.cx = {total.inertia.cx.x + piece.inertia.cx.x + m * (dd - d.x * d.x), total.inertia.cx.y + piece.inertia.cx.y - m * d.x * d.y, total.inertia.cx.z + piece.inertia.cx.z - m * d.x * d.z};
            total.inertia.cy = {total.inertia.cy.x + piece.inertia.cy.x - m * d.y * d.x, total.inertia.cy.y + piece.inertia.cy.y + m * (dd - d.y * d.y), total.inertia.cy.z + piece.inertia.cy.z - m * d.y * d.z};
            total.inertia.cz = {total.inertia.cz.x + piece.inertia.cz.x - m * d.z * d.x, total.inertia.cz.y + piece.inertia.cz.y - m * d.z * d.y, total.inertia.cz.z + piece.inertia.cz.z + m * (dd - d.z * d.z)};
        }
        massData[t] = total;
    }
    bodies_.assign(scene.props.size(), Body());
    for (size_t i = 0; i < scene.props.size(); i++) {
        const Scene::Prop& prop = scene.props[i];
        if (prop.type < 0 || prop.type >= (int)hulls.size() || hulls[prop.type].empty()) continue;
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
        for (b3HullData* hull : hulls[prop.type]) b3CreateHullShape(body, &shape, hull);
        b3Body_SetMassData(body, massData[prop.type]);   // (the body's origin is its centre of mass)
        bodies_[i].id = body;
    }
    colliderMode_ = 0;
    for (std::vector<b3HullData*>& pieces : hulls)
        for (b3HullData* hull : pieces) b3DestroyHull(hull);
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
        if (colliderMode_ != 0) show(scene, i, false);
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
        if (colliderMode_ != 0) show(scene, i, awake);
    }
}

void Box3dProps::show(Scene& scene, size_t index, bool awake) const {
    const Scene::Prop& prop = scene.props[index];
    if (prop.mover < 0 || prop.mover >= (int)scene.movers.size()) return;
    const Scene::PropType& type = scene.propTypes[prop.type];
    Mover& mover = scene.movers[prop.mover];
    const int solid = colliderMode_ == 2 && type.rawGeom[0] >= 0 ? type.rawGeom[awake ? 1 : 0] : type.colliderGeom[awake ? 1 : 0];
    if (colliderMode_ != 0 && solid >= 0) {
        mover.geom = solid;
        mover.lit = true;
    } else {
        mover.geom = type.geom;
        mover.lit = false;
    }
}

void Box3dProps::showColliders(Scene& scene, int mode) {
    colliderMode_ = mode;
    for (size_t i = 0; i < scene.props.size(); i++)
        show(scene, i, i < bodies_.size() && !B3_IS_NULL(bodies_[i].id) && b3Body_IsAwake(bodies_[i].id));
}

}  // namespace minigame01
