#include "minigame01/box3d_vehicle.h"
#include <algorithm>
#include <cmath>
#include "math/functions.h"

namespace minigame01 {

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kMaxContacts = 16;
constexpr int kMaxHullVertices = 64;   // of the hull of the car
// The engine applies the restitution of the hull to every surface (the car bounced on landing): impacts slower than this do not bounce.
constexpr float kBounceMinSpeed = 3.0f;         // m/s for a car of 4.3 m: the impact speed from which the hull bounces
constexpr float kBurnoutSlip = 25.0f;           // m/s of slip (for a car of 4.3 m) of a driven wheel for every time that the engine asks for more than the tyre holds
constexpr float kSidewaysSlideFraction = 0.9f;  // a tyre that uses this part of its grip sideways is sliding (traction control lets go)
constexpr float kSpinFollow = 25.0f;           // 1/s: how fast a wheel on the road comes to the speed the road gives it
constexpr float kAirSpinFollow = 4.0f;          // 1/s: how fast a wheel in the air comes to the speed that the engine asks for

constexpr float kWheelCastWidthFactor = 1.17f;      // the shape of the tyre is this much wider than the width the car's files give
constexpr float kWheelCastRounding = 0.125f;       // of the radius: the roundness of the tyre's edges (the cast shape is a cylinder swept by a sphere)
constexpr float kWheelCastCrown = 0.02f;           // of the radius: how much taller the tread is in the middle than at the sides (the rings in between follow a parabola)

inline b3Vec3 toBox(const Vec3f& v) { return {v.x, v.y, v.z}; }
inline Vec3f fromBox(const b3Vec3& v) { return Vec3f(v.x, v.y, v.z); }

using ContactSum = Box3dVehicle::ContactSum;

// The closest hit of a shape cast (see b3World_CastShape: the callback is called for every hit, in any order; returning the fraction clips
// the cast to it).
struct CastHit {
    bool hit = false;
    b3ShapeId shape = b3_nullShapeId;
    float fraction = 2.0f;
    b3Vec3 point = {0, 0, 0}, normal = {0, 1, 0};
    uint64_t material = 0;
};

float closestCastHit(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId, int, int, void* context) {
    if (fraction <= 1e-5f) return -1.0f;   // (the shape starts inside it: that is not the road under the wheel)
    CastHit* closest = static_cast<CastHit*>(context);
    if (fraction < closest->fraction) {
        closest->hit = true;
        closest->shape = shapeId;
        closest->fraction = fraction;
        closest->point = {point.x, point.y, point.z};
        closest->normal = normal;
        closest->material = userMaterialId;
    }
    return fraction;
}

// What the level does to the body over the physics step, from its contacts: the force (N), where it acts (the points weighted by their
// normal force), the normal force and the average normal.
ContactSum sumContacts(b3BodyId body, float stepSeconds) {
    ContactSum sum;
    b3ContactData contacts[kMaxContacts];
    const int count = b3Body_GetContactData(body, contacts, kMaxContacts);
    const Vec3f centre = fromBox(b3Body_GetWorldCenter(body));
    Vec3f pointSum, normalSum;
    float weightSum = 0;
    for (int c = 0; c < count; c++) {
        const bool isB = !B3_ID_EQUALS(b3Shape_GetBody(contacts[c].shapeIdA), body);  // the normal points from A to B, and pushes B along it
        const float side = isB ? 1.0f : -1.0f;
        for (int m = 0; m < contacts[c].manifoldCount; m++) {
            const b3Manifold& manifold = contacts[c].manifolds[m];
            if (manifold.pointCount == 0) continue;
            const Vec3f normal = fromBox(manifold.normal) * side;  // from the level into the body
            float normalImpulse = 0;
            for (int p = 0; p < manifold.pointCount; p++) {
                const b3ManifoldPoint& point = manifold.points[p];
                normalImpulse += point.totalNormalImpulse;  // (what the contact pushed with over the step)
                const Vec3f at = centre + fromBox(isB ? point.anchorB : point.anchorA);
                pointSum += at * point.totalNormalImpulse;
                weightSum += point.totalNormalImpulse;
            }
            if (normalImpulse <= 0) continue;
            sum.touching = true;
            sum.force += normal * (normalImpulse / stepSeconds);
            sum.normalForce += normalImpulse / stepSeconds;
            normalSum += normal * normalImpulse;
        }
    }
    if (weightSum > 0) sum.point = pointSum * (1.0f / weightSum);
    if (sum.touching) sum.normal = normalize(normalSum);
    return sum;
}

}  // namespace

Box3dVehicle::WheelShape Box3dVehicle::wheelShape(int i) const {
    WheelShape shape;
    const float halfWidth = std::max(0.5f * kWheelCastWidthFactor * wheelWidth_, 0.02f * wheelRadii[i]);
    shape.rounding = std::min(kWheelCastRounding * wheelRadii[i], halfWidth);
    shape.sideOffset = halfWidth - shape.rounding;
    // (the polygon of the points is a little smaller than its circle between the corners: the circle is made larger to match)
    const float ringRadius = (wheelRadii[i] - shape.rounding) * (1.0f + 0.5f * (1.0f - std::cos(kPi / kWheelCastSides)));
    const float crown = std::min(kWheelCastCrown * wheelRadii[i], 0.5f * shape.sideOffset);
    for (int k = 0; k < kWheelCastRings; k++) {
        const float across = 1.0f - 2.0f * (float)k / (float)(kWheelCastRings - 1);   // +1 at the +x side, -1 at the -x side
        shape.offset[k] = across * shape.sideOffset;
        shape.radius[k] = ringRadius - crown * across * across;
    }
    return shape;
}

void Box3dVehicle::destroyBodies() {
    if (!B3_IS_NULL(world) && b3World_IsValid(world) && !B3_IS_NULL(chassis)) b3DestroyBody(chassis);
    chassis = b3_nullBodyId;
}

void Box3dVehicle::init(const CarModel& m) {
    for (int i = 0; i < kWheelCount; i++) wheelRest[i] = m.wheelPos[i];
    bmin = m.boundsMin;
    bmax = m.boundsMax;
    hasHullBox = m.hasHullBox;
    hullBoxMin = m.hullBoxMin;
    hullBoxMax = m.hullBoxMax;
    hullPoints_ = m.hullPoints;
    wheelWidth_ = m.wheelWidth;
    modelHullIndices_ = m.hullTriangleIndices;
    centreOfMass = Vec3f();
    applyTuning();
    reset(Vec3f(), 0);
}

// How much stiffer than its normal spring the suspension is at this length: the bump stop stiffens smoothly over its zone (1 above it).
float Box3dVehicle::bumpStiffnessAt(float suspensionLength) const {
    const float depth = clamp((bumpZone_ - suspensionLength) / std::max(1e-4f, bumpZone_), 0.0f, 1.0f);  // 0: above the zone, 1: at the stop
    return 1.0f + (tuning().bumpStopStiffness - 1.0f) * depth * depth;
}

Vec3f Box3dVehicle::invInertia(const Vec3f& t) const {
    const Vec3f b(dot(t, axes[0]) / inertia.x, dot(t, axes[1]) / inertia.y, dot(t, axes[2]) / inertia.z);
    return axes[0] * b.x + axes[1] * b.y + axes[2] * b.z;
}

// Takes the values of the car from the settings and makes its body from them. Called when the car is set up and whenever the settings
// change: the car stays where it is (the new body gets the pose and the speed of the old one).
void Box3dVehicle::applyTuning() {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    toy_ = C.toy > 0.5f;
    sizeScale_ = C.sizeScale;
    gravity_ = C.gravity;
    engineForce_ = C.engineForce;
    enginePower_ = C.enginePower;
    grip_ = C.grip;
    dragCoefficient_ = C.drag;
    afterburnerForce_ = C.afterburnerForce;
    kangarooSpeed_ = C.kangarooSpeed;
    mass = C.mass;
    suspensionLengthMax_ = C.suspensionLengthMax;
    suspensionLengthAtRest_ = C.suspensionLengthAtRest;
    suspensionLengthInLoadedModel_ = C.suspensionLengthInLoadedModel;
    bumpZone_ = C.bumpStopZone;
    springStiffness_ = C.suspensionSpringK;
    damping_ = C.suspensionDamperCoefficient;
    // The length of the spring in isolation: the one at which the car's weight, shared by the wheels, compresses it to the rest length of
    // the suspension (so the car stands at its rest pose, whatever its mass, gravity and spring).
    suspensionSpringNaturalLength_ = suspensionLengthAtRest_ + mass * gravity_ / (kWheelCount * std::max(1.0f, springStiffness_) * bumpStiffnessAt(suspensionLengthAtRest_));
    wheelRadii[0] = C.wheelRadius0;
    wheelRadii[1] = C.wheelRadius1;
    wheelRadii[2] = C.wheelRadius2;
    wheelRadii[3] = C.wheelRadius3;
    centreOfMass = Vec3f(C.centreOfMassX, C.centreOfMassY, C.centreOfMassZ);
    const float w = bmax.x - bmin.x, h = bmax.y - bmin.y, l = bmax.z - bmin.z;
    inertia = Vec3f(mass / 12.0f * (h * h + l * l), mass / 12.0f * (w * w + l * l), mass / 12.0f * (w * w + h * h) * P.rollInertiaFactor);
    if (B3_IS_NULL(world)) return;

    // the pose and the speed to keep
    const bool existed = !B3_IS_NULL(chassis);
    b3Transform pose = {{0, 0, 0}, b3Quat_identity};
    b3Vec3 linear = {0, 0, 0}, angular = {0, 0, 0};
    if (existed) {
        const b3WorldTransform now = b3Body_GetTransform(chassis);
        pose.p = now.p;
        pose.q = now.q;
        linear = b3Body_GetLinearVelocity(chassis);
        angular = b3Body_GetAngularVelocity(chassis);
        destroyBodies();
    }
    b3World_SetGravity(world, {0.0f, -gravity_, 0.0f});
    b3World_SetMaximumLinearSpeed(world, P.maxSpeed);
    b3World_SetRestitutionThreshold(world, kBounceMinSpeed * sizeScale_);  // slower impacts do not bounce at all

    // the body: the convex hull of the car's files (else of its body mesh, else the box of its bounds), with the mass and the inertia of the car
    b3BodyDef def = b3DefaultBodyDef();
    def.type = b3_dynamicBody;
    def.position = pose.p;
    def.rotation = pose.q;
    def.enableSleep = false;
    chassis = b3CreateBody(world, &def);
    // The collider is the convex hull that the engine makes of the points of the car's hull (or of its body), at most kMaxHullVertices
    // points of it. A car without any points gets the hull of the corners of its box.
    std::vector<b3Vec3> points;
    for (const Vec3f& p : hullPoints_) points.push_back(toBox(p));
    if (points.size() < 4) {
        points.clear();
        const Vec3f lo = hasHullBox ? hullBoxMin : bmin, hi = hasHullBox ? hullBoxMax : bmax;
        for (int corner = 0; corner < 8; corner++) points.push_back({(corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z});
    }
    b3HullData* hull = b3CreateHull(points.data(), (int)points.size(), kMaxHullVertices);
    b3ShapeDef shape = b3DefaultShapeDef();
    shape.baseMaterial.friction = P.floorFriction;   // against the level: this times the surface's multiplier
    shape.baseMaterial.restitution = C.hullWallBounce;
    shape.filter.categoryBits = kCarCategory;
    if (hull) {
        b3CreateHullShape(chassis, &shape, hull);
        if (!modelHullIndices_.empty() && hullPoints_.size() >= 4) {   // the faces of the car's own file, as they are
            hullVertices_ = hullPoints_;
            hullIndices_ = modelHullIndices_;
        } else {
            hullTriangles(hull, hullVertices_, hullIndices_);
        }
        b3DestroyHull(hull);
    } else {   // (the points could not make a hull: the box of the car)
        const Vec3f lo = hasHullBox ? hullBoxMin : bmin, hi = hasHullBox ? hullBoxMax : bmax;
        const Vec3f half = (hi - lo) * 0.5f, middle = (hi + lo) * 0.5f;
        b3BoxHull box = b3MakeOffsetBoxHull(half.x, half.y, half.z, toBox(middle));
        b3CreateHullShape(chassis, &shape, &box.base);
        hullVertices_.clear();
        hullIndices_.clear();
    }
    b3MassData data;
    data.mass = mass;
    data.center = toBox(centreOfMass);
    data.inertia.cx = {inertia.x, 0, 0};
    data.inertia.cy = {0, inertia.y, 0};
    data.inertia.cz = {0, 0, inertia.z};
    b3Body_SetMassData(chassis, data);
    b3Body_SetLinearVelocity(chassis, linear);
    b3Body_SetAngularVelocity(chassis, angular);
    readState();
}

void Box3dVehicle::reset(const Vec3f& groundPos, float yaw) {
    if (B3_IS_NULL(chassis)) return;
    const b3Quat q = b3MakeQuatFromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
    // the car's origin so that the wheels touch the road with the suspension at its rest length
    float lift = -1e30f;
    for (int i = 0; i < kWheelCount; i++) lift = std::max(lift, wheelRadii[i] - (wheelRest[i].y + suspensionLengthInLoadedModel_ - suspensionLengthAtRest_));
    const b3Vec3 origin = toBox(groundPos + Vec3f(0, lift + 0.02f * sizeScale_, 0));
    b3Body_SetTransform(chassis, origin, q);
    b3Body_SetLinearVelocity(chassis, {0, 0, 0});
    b3Body_SetAngularVelocity(chassis, {0, 0, 0});
    for (int i = 0; i < kWheelCount; i++) {
        wheels[i] = Wheel();
        wheels[i].centre = wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - suspensionLengthAtRest_, 0);
        wheelSpeed_[i] = 0;
        wheelDriveForce_[i] = 0;
    }
    steerAngle = 0;
    groundedCount = 0;
    drift_ = impact_ = 0;
    readState();
}

void Box3dVehicle::liftAndRoll(float metres) {
    if (B3_IS_NULL(chassis)) return;
    const b3WorldTransform now = b3Body_GetTransform(chassis);
    const b3Quat roll = b3MakeQuatFromAxisAngle({0.0f, 0.0f, 1.0f}, kPi * 0.5f);   // about the car's own nose axis
    const b3Quat turned = b3MulQuat(now.q, roll);
    const b3Vec3 centre = b3Body_GetWorldCenter(chassis);
    const b3Vec3 newCentre = {centre.x, centre.y + metres, centre.z};
    b3Body_SetTransform(chassis, b3Sub(newCentre, b3RotateVector(turned, toBox(centreOfMass))), turned);
    readState();
}

Mat4f Box3dVehicle::modelMatrix() const {
    Mat4f m = Mat4f::identity();
    const Vec3f origin = pos - (axes[0] * centreOfMass.x + axes[1] * centreOfMass.y + axes[2] * centreOfMass.z);
    m.m[0] = axes[0].x; m.m[1] = axes[0].y; m.m[2] = axes[0].z;
    m.m[4] = axes[1].x; m.m[5] = axes[1].y; m.m[6] = axes[1].z;
    m.m[8] = axes[2].x; m.m[9] = axes[2].y; m.m[10] = axes[2].z;
    m.m[12] = origin.x;
    m.m[13] = origin.y;
    m.m[14] = origin.z;
    return m;
}

// The forces that the level puts on the hull, one per contact point (for the grey arrows of the debug display).
void Box3dVehicle::readHullArrows(float stepSeconds) {
    int count = 0;
    b3ContactData contacts[kMaxContacts];
    const int contactCount = b3Body_GetContactData(chassis, contacts, kMaxContacts);
    const Vec3f centre = fromBox(b3Body_GetWorldCenter(chassis));
    for (int c = 0; c < contactCount; c++) {
        const bool isB = !B3_ID_EQUALS(b3Shape_GetBody(contacts[c].shapeIdA), chassis);   // the normal points from A to B, and pushes B along it
        const float side = isB ? 1.0f : -1.0f;
        for (int m = 0; m < contacts[c].manifoldCount; m++) {
            const b3Manifold& manifold = contacts[c].manifolds[m];
            for (int p = 0; p < manifold.pointCount && count < kMaxHullArrows; p++) {
                const b3ManifoldPoint& point = manifold.points[p];
                if (point.totalNormalImpulse <= 0) continue;
                hullArrowPoint_[count] = centre + fromBox(isB ? point.anchorB : point.anchorA);
                hullArrowForce_[count] = fromBox(manifold.normal) * (side * point.totalNormalImpulse / stepSeconds);
                count++;
            }
        }
    }
    for (int i = count; i < kMaxHullArrows; i++) hullArrowForce_[i] = Vec3f();
    hullArrowCount_ = count;
}

void Box3dVehicle::readState() {
    if (B3_IS_NULL(chassis)) return;
    const b3WorldTransform tf = b3Body_GetTransform(chassis);
    axes[0] = fromBox(b3RotateVector(tf.q, {1.0f, 0.0f, 0.0f}));
    axes[1] = fromBox(b3RotateVector(tf.q, {0.0f, 1.0f, 0.0f}));
    axes[2] = fromBox(b3RotateVector(tf.q, {0.0f, 0.0f, 1.0f}));
    pos = fromBox(b3Body_GetWorldCenter(chassis));
    vel = fromBox(b3Body_GetLinearVelocity(chassis));
    angVel = fromBox(b3Body_GetAngularVelocity(chassis));
}

void Box3dVehicle::step(float dt, const VehicleInput& in) {
    if (B3_IS_NULL(chassis)) return;
    const Params& P = tuning();
    dt = std::min(dt, P.vehicleMaxFrameSeconds);
    const int n = std::max(1, (int)std::ceil(dt / P.maxPhysicsStep));
    const float h = dt / (float)n;
    impact_ = 0;
    if (in.jump) {  // an impulse, once per press: straight up whatever the car's attitude
        const b3Vec3 v = b3Body_GetLinearVelocity(chassis);
        b3Body_SetLinearVelocity(chassis, {v.x, v.y + kangarooSpeed_, v.z});
    }
    for (int i = 0; i < n; i++) substep(h, in);
}

// One physics step of `stepSeconds`: the wheels are ray cast from the car's pose, the forces of the suspensions and the tyres (and the drag,
// the downforce and the rockets) are put on the body, and Box3D moves it, against the level with its hull.
void Box3dVehicle::substep(float stepSeconds, const VehicleInput& in) {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    readState();
    const Vec3f rightAxis = axes[0], upAxis = axes[1], forwardAxis = axes[2];
    const float speed = length(vel);

    // --- steering: the angle that the grip allows at this speed (the wheels turn to it smoothly): the one of a circle with no slip at the
    // most that the tyres can turn the car, plus the slip at which the tyre gives all its grip
    const float gripAccelerationLimitForSteering = C.steerAngleCentrifugalLimitFactor * grip_ * gravity();
    const float wheelBase = std::abs(0.5f * (wheelRest[0].z + wheelRest[1].z) - 0.5f * (wheelRest[2].z + wheelRest[3].z));
    const float minTurnRadiusAtCurrentSpeed = speed * speed / gripAccelerationLimitForSteering;
    const float steerAngleFromMinTurnRadius = std::atan2(wheelBase, minTurnRadiusAtCurrentSpeed);
    const float steerAnglePlateauSlip = P.tyreSlipCurve > 0.5f ? std::atan(P.tyreSlipFull) : 0.0f;
    const float steerAngleMaxAtCurrentSpeed = smoothlimit(steerAngleFromMinTurnRadius + steerAnglePlateauSlip, C.steerAngleMax, C.steerAngleCentrifugalKneeFactor);
    const float targetSteerAngle = clamp(in.steer, -1.0f, 1.0f) * steerAngleMaxAtCurrentSpeed;
    steerAngle += (targetSteerAngle - steerAngle) * std::min(1.0f, stepSeconds * C.steerResponse);

    // --- forces on the whole car at its centre of mass: air drag, downforce, the rockets (gravity is the engine's)
    dragForce_ = vel * (-dragCoefficient_ * speed);
    // The downforce grows like the square of the speed at first and then in proportion to it: rate * (sqrt(v^2 + knee^2) - knee), smooth all
    // the way and without the runaway of a square at high speed.
    const float downforceMagnitude = C.downforceRate * (std::sqrt(speed * speed + C.downforceKnee * C.downforceKnee) - C.downforceKnee);
    downforce_ = upAxis * -downforceMagnitude;
    thrustForce_ = Vec3f();
    if (in.afterburner) thrustForce_ += forwardAxis * afterburnerForce_;
    if (in.sideThrust) thrustForce_ += rightAxis * afterburnerForce_;
    gravityForce_ = Vec3f(0, -gravity() * mass, 0);

    // --- the wheels
    groundedCount = 0;
    float largestSlipAngle = 0;  // radians, over the wheels on the ground: how sideways the car slides (see the drift measure below)
    // Three passes over the wheels: 1. where the road is under each wheel, and the bottom-out stops (they change the speed and position of
    // the car, so they come before any force is worked out); 2. the suspension forces, from the speeds that left; 3. the tyres, which
    // need all the suspension forces (the "weighted" grip shares the weight of the car by them).
    struct WheelContact {
        bool grounded = false;
        float suspensionLength = 0;   // from the mount to the wheel centre; the maximum when nothing is under the wheel
        float roadFriction = 1.0f;    // the surface's tyre friction multiplier (Attribute files: grass 0.8, road 1.0, ...)
        Vec3f groundNormal;           // unit normal of the road where the ray hit
        Vec3f contactPoint;           // world position where the tyre touches the road
        float suspensionForce = 0;    // N
        float bottomedBy = 0;         // m past the end of the suspension's travel (0: not that far)
        b3BodyId groundBody = b3_nullBodyId;   // what the tyre stands on, when that is a body that moves (a movable object): the tyre pushes on it too
        Vec3f groundVelocity;                  // m/s, how fast that body moves where the tyre touches it
    } contact[kWheelCount];
    // Where the forces of a wheel act on the car, from the centre of mass: the suspension's (spring, damper, bottom-out) and the tyre's
    // (engine, brakes, grip) alike. Above the point where the tyre touches the road, by a distance and by a part of the tyre's radius.
    auto forceLeverOf = [&](int i, const Vec3f& contactPoint) {
        return contactPoint - pos + upAxis * (C.tyreForceLiftOffset + 2.0f * C.tyreForceLiftRadiusFraction * wheelRadii[i]);
    };
    // The upper end of a suspension in the world, from the car's position now.
    auto suspensionMountOf = [&](int i) {
        const Vec3f wheelOffsetFromCentreOfMass = wheelRest[i] - centreOfMass;  // in car space
        return pos + rightAxis * wheelOffsetFromCentreOfMass.x + upAxis * (wheelOffsetFromCentreOfMass.y + suspensionLengthInLoadedModel_) +
               forwardAxis * wheelOffsetFromCentreOfMass.z;
    };
    // How fast the length of a suspension changes (m/s, positive: it extends): the rate at which the distance the ray travels to the road
    // changes, as its mount moves, the ray turns with the car and the road lies where it lies. It is not the speed of the mount along the
    // car's up axis: a pitched car that drives along a flat road moves along that axis too (speed * sin(pitch)) with the suspension
    // not moving at all, and the damper would push the car down when it goes forwards and lift it when it goes backwards.
    auto suspensionExtensionRate = [&](int i, const WheelContact& c, float rayDistance) {
        const Vec3f mountVelocity = vel + cross(angVel, suspensionMountOf(i) - pos) - c.groundVelocity;
        const Vec3f roadNormal = dot(c.groundNormal, upAxis) >= 0 ? c.groundNormal : c.groundNormal * -1.0f;  // towards the car
        const float alignment = std::max(0.2f, dot(roadNormal, upAxis));  // (a ray almost along the road: not divided by about zero)
        return (dot(roadNormal, mountVelocity) + rayDistance * dot(roadNormal, cross(angVel, upAxis * -1.0f))) / alignment;
    };

    // --- pass 1: the road under the wheels (Box3D shape casts of the tyre against the level, not the car), and the bottom-out
    b3QueryFilter rayFilter = b3DefaultQueryFilter();
    rayFilter.maskBits = ~kCarCategory;
    const Vec3f positionBefore = pos;
    bool bottomedOut = false;
    for (int i = 0; i < kWheelCount; i++) {
        Wheel& wheel = wheels[i];
        WheelContact& c = contact[i];
        c.suspensionLength = suspensionLengthMax_;
        wheel.grounded = false;
        wheel.load = 0;
        // The suspension is the way of the tyre from its upper mount straight down (along the car's down axis): the tyre, a rounded cylinder
        // as wide and as tall as the real one, is cast down from there, and where it meets the road, and how much of the suspension is
        // left, tells how far the spring is compressed. (A ray would find the road under the middle of the wheel only: the wheel
        // would go through a sharp edge that its side meets first.)
        const Vec3f suspensionMount = suspensionMountOf(i);
        // The cast starts a bit above the mount (more for the cars with more travel): a road surface that is higher than the mount
        // (a curb, a wall the car drives against) is then still found from above, instead of the tyre starting inside it or below it.
        const float rayLift = C.rayLift;
        const Vec3f castOrigin = suspensionMount + upAxis * rayLift;   // where the middle of the tyre starts
        const float castLength = suspensionLengthMax_ + rayLift;       // fully extended suspension
        const float wheelSteer = i < 2 ? steerAngle : 0.0f;
        const Vec3f heading = forwardAxis * std::cos(wheelSteer) + rightAxis * std::sin(wheelSteer);
        const Vec3f axle = rightAxis * std::cos(wheelSteer) - forwardAxis * std::sin(wheelSteer);
        b3Vec3 tyrePoints[kWheelCastRings * kWheelCastSides];
        {
            const WheelShape shape = wheelShape(i);
            for (int k = 0; k < kWheelCastSides; k++) {
                const float angle = 2.0f * kPi * (float)k / (float)kWheelCastSides;
                const Vec3f direction = upAxis * std::cos(angle) + heading * std::sin(angle);
                for (int ring = 0; ring < kWheelCastRings; ring++)
                    tyrePoints[kWheelCastRings * k + ring] = toBox(direction * shape.radius[ring] + axle * shape.offset[ring]);
            }
            const b3ShapeProxy proxy = {tyrePoints, kWheelCastRings * kWheelCastSides, shape.rounding};
            CastHit closest;
            b3World_CastShape(world, toBox(castOrigin), &proxy, toBox(upAxis * -castLength), rayFilter, closestCastHit, &closest);
            if (!closest.hit) continue;
            c.groundNormal = normalize(fromBox(closest.normal));
            c.roadFriction = level_ ? level_->surfaceFriction(closest.material) : 1.0f;
            const float rawLength = closest.fraction * castLength - rayLift;   // from the mount to the middle of the tyre; < 0: past the stop
            c.suspensionLength = std::max(0.0f, rawLength);
            if (c.suspensionLength >= suspensionLengthMax_) continue;
            // where the tyre touches the road: the point of the cast (the forces act above it, see forceLeverOf)
            c.contactPoint = fromBox(closest.point);
            c.bottomedBy = -rawLength;
            const b3BodyId ground = b3Shape_GetBody(closest.shape);
            if (b3Body_GetType(ground) == b3_dynamicBody) {
                c.groundBody = ground;
                c.groundVelocity = fromBox(b3Body_GetWorldPointVelocity(ground, toBox(c.contactPoint)));
            }
        }
        wheel.grounded = c.grounded = true;
        groundedCount++;
        const float rayHitDistance = c.suspensionLength + wheelRadii[i];
        // Bottoming out: the wheel is past the end of the suspension's travel. That end is a solid stop, not a spring: the part of the
        // speed that still presses the tyre into the road is taken away at once (an inelastic collision, nothing comes back), and
        // the car is lifted out of the overlap without any speed, so it does not bounce.
        const float bottomedBy = c.bottomedBy;  // m past the stop, > 0 when bottomed out
        if (bottomedBy > 0) {
            const Vec3f pressLever = forceLeverOf(i, c.contactPoint);   // where the stop pushes the car, from the centre of mass
            const float pressSpeed = suspensionExtensionRate(i, c, rayHitDistance);  // < 0: the suspension is shortening: moving into the road
            if (pressSpeed < 0) {
                const Vec3f turning = cross(pressLever, upAxis);
                const float stopImpulse = -pressSpeed / (1.0f / mass + dot(turning, invInertia(turning)));
                vel += upAxis * (stopImpulse / mass);
                angVel += invInertia(cross(pressLever, upAxis * stopImpulse));
                impact_ = std::max(impact_, -pressSpeed);
                bottomedOut = true;
            }
            pos += upAxis * (bottomedBy * P.bottomOutPushOut);
        }
    }
    if (bottomedOut || length(pos - positionBefore) > 0) {   // the stops changed the speed and the position: the body has them from here on
        const b3WorldTransform now = b3Body_GetTransform(chassis);
        const Vec3f moved = pos - positionBefore;
        b3Body_SetTransform(chassis, {now.p.x + moved.x, now.p.y + moved.y, now.p.z + moved.z}, now.q);
        b3Body_SetLinearVelocity(chassis, toBox(vel));
        b3Body_SetAngularVelocity(chassis, toBox(angVel));
    }

    // --- pass 2: the suspension forces
    float suspensionForceSum = 0;  // N, over the wheels on the ground
    Vec3f totalTorque;             // N m, about the centre of mass (of the forces of the wheels: for the debug display)
    for (int i = 0; i < kWheelCount; i++) {
        wheels[i].suspensionForceOrigin = wheels[i].tyreForceOrigin = pos + forceLeverOf(i, contact[i].contactPoint);
        wheels[i].centre = wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - contact[i].suspensionLength, 0);   // for drawing: lower when the suspension is extended
        if (!contact[i].grounded) continue;
        WheelContact& c = contact[i];
        const Vec3f forceLever = forceLeverOf(i, c.contactPoint);   // where the suspension pushes the car, from the centre of mass
        const float extensionRate = suspensionExtensionRate(i, c, c.suspensionLength + wheelRadii[i]);  // m/s, positive: the suspension extends
        // spring + damper (stiffer at the bump stop); a suspension cannot pull the car down, so never negative
        // The spring is free a little longer when the downforce presses on it, so the ride height stays (a fraction of the compression
        // the downforce would make in a quarter of the car's spring); the bump stop stiffens smoothly over its zone.
        const float suspensionSpringNaturalLengthNow = suspensionSpringNaturalLength_ + C.downforceHeightCompensation * downforceMagnitude / (kWheelCount * std::max(1.0f, springStiffness_));
        const float springCompression = suspensionSpringNaturalLengthNow - c.suspensionLength;
        const float bumpStiffness = bumpStiffnessAt(c.suspensionLength);
        c.suspensionForce = std::max(0.0f, springStiffness_ * springCompression * bumpStiffness + damping_ * -extensionRate);
        wheels[i].load = c.suspensionForce;  // N: how hard the tyre is pressed on the road
        suspensionForceSum += c.suspensionForce;
        // The road pushes the tyre perpendicular to its surface, at the point where the wheel's forces act (not along the suspension: a pitched car on a
        // flat road would then be pushed forward by the horizontal part of the suspension force). The spring, the damper and the
        // bottom-out above still work along the car's up axis, the direction the suspension moves in.
        const Vec3f roadNormal = dot(c.groundNormal, upAxis) >= 0 ? c.groundNormal : c.groundNormal * -1.0f;  // towards the car
        const Vec3f suspensionForceVector = roadNormal * c.suspensionForce;
        b3Body_ApplyForce(chassis, toBox(suspensionForceVector), toBox(pos + forceLever), true);
        if (!B3_IS_NULL(c.groundBody)) b3Body_ApplyForce(c.groundBody, toBox(suspensionForceVector * -1.0f), toBox(c.contactPoint), true);
        totalTorque += cross(forceLever, suspensionForceVector);
        wheels[i].force = suspensionForceVector;
        wheels[i].contactPoint = c.contactPoint;
    }

    // --- pass 3: the tyres: the engine on the rear wheels (half each), the brakes (and the handbrake) on the ones they act on, the grip.
    // Every wheel decides for itself what the pedals mean, by how it moves along the road: the accelerator brakes a wheel that runs backwards
    // and drives it otherwise; the brake pedal brakes a wheel that runs forwards, and drives a driven one backwards otherwise. A wheel in the
    // air has no brake to use: it spins up (the handbrake does not block it either).
    constexpr float kMinPowerSpeed = 1e-3f;  // m/s: only so that power / speed is finite when the car stands still
    const float handbrakeInput = in.handbrake;  // 0..1
    for (int i = 0; i < kWheelCount; i++) {
        Wheel& wheel = wheels[i];
        const WheelContact& c = contact[i];
        const bool isFrontWheel = i < 2, isRearWheel = !isFrontWheel;  // wheels 0, 1 are the front ones, 2, 3 the rear (drive) ones
        const bool handbrakeActsHere = isRearWheel || C.handbrakeAllWheels > 0.5f;
        const float wheelSteerAngle = isFrontWheel ? steerAngle : 0.0f;
        const float maxSpin = (C.engineTaperStartSpeed + C.engineTaperLength) / wheelRadii[i];
        wheel.steer = wheelSteerAngle;
        wheelDriveForce_[i] = 0;

        // what the pedals ask of this wheel: the push of the engine (N at the tyre, negative: reverse) and the brake pedal
        auto intent = [&](bool touching, float rolling, float surfaceSpeed, float& driveDemand, float& brakeInput) {
            driveDemand = 0;
            brakeInput = 0;
            if (in.accel > P.controlMinPedal) {
                brakeInput = in.brake;
                if (touching && rolling < -1.0f) brakeInput = std::max(brakeInput, in.accel);
                else if (isRearWheel) driveDemand = 0.5f * in.accel * std::min(engineForce_, enginePower_ / std::max(std::fabs(surfaceSpeed), kMinPowerSpeed));
            } else if (in.brake > P.controlMinPedal) {
                if (touching && rolling >= 1.0f) brakeInput = in.brake;
                else if (isRearWheel) driveDemand = -0.5f * in.brake * std::min(engineForce_ * C.reverseForceFraction,
                                                                              enginePower_ * C.reversePowerFraction / std::max(std::fabs(surfaceSpeed), kMinPowerSpeed));
            }
            if (!touching) brakeInput = 0;
            // the engine's push fades out over a speed range, so it has a soft top speed of its own (the afterburner is not affected)
            if (std::fabs(surfaceSpeed) > C.engineTaperStartSpeed && driveDemand * surfaceSpeed > 0)
                driveDemand *= std::max(0.0f, 1.0f - (std::fabs(surfaceSpeed) - C.engineTaperStartSpeed) / C.engineTaperLength);
        };

        if (!c.grounded) {   // in the air: the wheel turns as the engine asks, and slows down by itself
            float driveDemand, brakeInput;
            intent(false, 0.0f, wheelSpeed_[i] * wheelRadii[i], driveDemand, brakeInput);
            const float target = maxSpin * clamp(driveDemand / (0.5f * engineForce_), -1.0f, 1.0f);
            if (driveDemand != 0.0f) wheelSpeed_[i] += (target - wheelSpeed_[i]) * std::min(1.0f, stepSeconds * kAirSpinFollow);
            else wheelSpeed_[i] *= 1.0f - 0.3f * stepSeconds;
            wheel.spin += wheelSpeed_[i] * stepSeconds;
            wheel.slip = 0;
            wheel.lateralForce = wheel.lateralLimit = 0;
            wheel.longitudinalForce = wheel.frictionLimit = 0;
            wheel.tyreForce = Vec3f();
            wheel.force = Vec3f();
            wheel.sliding = false;
            continue;
        }

        const Vec3f contactOffset = c.contactPoint - pos;  // from the centre of mass to where the tyre touches the road
        const Vec3f tyreLever = forceLeverOf(i, c.contactPoint);   // from the centre of mass to where the tyre's force acts (above the road, see tyreForceLiftOffset)
        const float suspensionForce = c.suspensionForce;
        const Vec3f groundNormal = c.groundNormal;
        // The wheel's own axes on the road: where it rolls (the front wheels are turned by the steering, then the
        // direction is projected onto the road) and the direction across that.
        const Vec3f wheelHeading = forwardAxis * std::cos(wheelSteerAngle) + rightAxis * std::sin(wheelSteerAngle);
        const Vec3f wheelForwardOnRoad = normalize(wheelHeading - groundNormal * dot(wheelHeading, groundNormal));
        const Vec3f wheelSidewaysOnRoad = normalize(cross(groundNormal, wheelForwardOnRoad));
        const Vec3f contactVelocity = vel + cross(angVel, contactOffset) - c.groundVelocity;  // how fast the road moves under the tyre
        const float longitudinalSpeed = dot(contactVelocity, wheelForwardOnRoad);  // along the wheel's rolling direction
        const float lateralSpeed = dot(contactVelocity, wheelSidewaysOnRoad);      // across it: this is skidding
        // The mass this tyre moves when it pushes along a direction at its contact point: the whole car, but a push off the centre
        // of mass also turns the car, which lets the point move more easily. That is what the stopping forces below are made for
        // (the force that would cancel the point's speed within one step); and only a part of it is asked for (tyreStopFraction), as the
        // other wheels push too.
        auto effectiveMass = [&](const Vec3f& direction) {
            const Vec3f turning = cross(tyreLever, direction);
            const float inverseMass = 1.0f / mass + dot(turning, invInertia(turning));
            return 1.0f / std::max(inverseMass, 1e-6f);
        };
        const float forwardStopMass = effectiveMass(wheelForwardOnRoad) * P.tyreStopFraction;
        const float sidewaysStopMass = effectiveMass(wheelSidewaysOnRoad) * P.tyreStopFraction;

        float driveDemand, brakeInput;
        intent(true, longitudinalSpeed, longitudinalSpeed, driveDemand, brakeInput);

        // Force along the rolling direction: the engine, the brakes, rolling resistance.
        float longitudinalForce = driveDemand;
        // The load a tyre grips with: between the weight of the car shared equally and what the suspensions carry (the distribution),
        // and between the car's static weight and the real total of the suspension forces (the scaling).
        const float tyreLoadStatic = mass * gravity() / kWheelCount;  // N: the car's weight shared by the wheels
        const float tyreLoadDynamic = suspensionForce;
        const float tyreLoadDynamicAverage = suspensionForceSum / kWheelCount;
        const float tyreLoadDistributionSensitivity = C.tyreLoadDistributionSensitivity;
        const float tyreLoadScalingSensitivity = C.tyreLoadScalingSensitivity;
        const float tyreLoadDynamicScaledToTotalStatic = tyreLoadStatic * (tyreLoadDynamicAverage < 1e-4 ? 1 : tyreLoadDynamic / tyreLoadDynamicAverage);
        const float frictionLoad = mix(
            mix(tyreLoadStatic, tyreLoadDynamicScaledToTotalStatic, tyreLoadDistributionSensitivity),
            mix(tyreLoadDynamicAverage, tyreLoadDynamic, tyreLoadDistributionSensitivity),
            tyreLoadScalingSensitivity
        );
        const float frictionLimit = grip_ * (1.0f + (c.roadFriction - 1.0f) * C.tyreSurfaceFrictionEffectFactor) * frictionLoad;  // N: the most the tyre can push with, in any direction
        float brakingForce = brakeInput * P.brakeForce * frictionLimit;
        if (handbrakeActsHere) brakingForce = std::max(brakingForce, handbrakeInput * C.handbrakeBrake * frictionLimit);
        const float rollingDirection = longitudinalSpeed > 0 ? 1.0f : -1.0f;  // brakes and resistance act against the motion
        const float maxBrakingForce = std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds;  // never brake past a standstill
        longitudinalForce -= rollingDirection * std::min(brakingForce, maxBrakingForce) * std::min(1.0f, std::fabs(longitudinalSpeed) * 2.0f);
        const float rollingResistanceForce = std::min(P.rollResist * suspensionForce, std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds);
        longitudinalForce -= rollingDirection * rollingResistanceForce;

        // Force across the wheel: the force that would stop the sideways slide within one step, limited by the grip
        // (the wheels lose grip with the handbrake); a tyre over the limit slides and keeps only part of it.
        const float handbrakeGripFactor = (handbrakeActsHere && handbrakeInput > 0.05f) ? 1.0f - C.handbrakeGripLoss * handbrakeInput : 1.0f;
        const float lateralFrictionLimit = frictionLimit * handbrakeGripFactor;
        float lateralForce = -lateralSpeed * sidewaysStopMass / stepSeconds * P.lateralStiffness;
        bool isSliding = std::fabs(lateralForce) > lateralFrictionLimit;
        if (P.tyreSlipCurve > 0.5f) {
            // The game's slip curve: the slip is the sideways speed relative to the speed of the contact point (at least a
            // floor), and the grip grows with it up to a plateau. The force is capped at what stops the slide in one step.
            const float contactSpeed = std::max(std::sqrt(longitudinalSpeed * longitudinalSpeed + lateralSpeed * lateralSpeed), C.slipFloorSpeed);
            const float slipFraction = std::fabs(lateralSpeed) / contactSpeed;
            const float gripFraction = slipFraction < P.tyreSlipKnee
                ? P.tyreSlipKneeValue * slipFraction / P.tyreSlipKnee
                : std::min(1.0f, P.tyreSlipKneeValue + (1.0f - P.tyreSlipKneeValue) * (slipFraction - P.tyreSlipKnee) / (P.tyreSlipFull - P.tyreSlipKnee));
            const float stopForce = std::fabs(lateralSpeed) * sidewaysStopMass / stepSeconds;
            lateralForce = -(lateralSpeed > 0 ? 1.0f : -1.0f) * std::min(gripFraction * lateralFrictionLimit, stopForce);
            isSliding = gripFraction >= 1.0f;
        } else if (isSliding) {
            lateralForce = (lateralForce > 0 ? 1.0f : -1.0f) * lateralFrictionLimit * P.slideGrip;
        }
        // the friction circle: what the sideways force uses of the grip is not left for pushing along the wheel
        const float longitudinalFrictionLimit = std::sqrt(std::max(0.0f, frictionLimit * frictionLimit - lateralForce * lateralForce));
        longitudinalForce = clamp(longitudinalForce, -longitudinalFrictionLimit, longitudinalFrictionLimit);

        const Vec3f tyreForce = wheelForwardOnRoad * longitudinalForce + wheelSidewaysOnRoad * lateralForce;
        b3Body_ApplyForce(chassis, toBox(tyreForce), toBox(pos + tyreLever), true);
        if (!B3_IS_NULL(c.groundBody)) b3Body_ApplyForce(c.groundBody, toBox(tyreForce * -1.0f), toBox(c.contactPoint), true);
        totalTorque += cross(tyreLever, tyreForce);
        wheel.tyreForce = tyreForce;  // (the suspension force was put in wheel.force in pass 2, with the point both act at)
        wheel.lateralForce = lateralForce;
        wheel.longitudinalForce = longitudinalForce;
        wheel.frictionLimit = frictionLimit;
        wheel.lateralLimit = lateralFrictionLimit;
        wheel.sliding = isSliding;
        wheel.slip = std::atan2(std::fabs(lateralSpeed), std::fabs(longitudinalSpeed) + 1.0f);  // slip angle, radians (+1: no jump near standstill)
        largestSlipAngle = std::max(largestSlipAngle, wheel.slip);
        wheelDriveForce_[i] = clamp(driveDemand, -longitudinalFrictionLimit, longitudinalFrictionLimit);

        // How the wheel turns (the wheels have no mass: this is the speed they would have, for the look of it, and it goes by what the engine
        // and the brakes ask for): on the road it follows the road, a brake blocks it (the handbrake too), and it spins faster when the engine
        // asks for more than the tyre holds.
        // Traction control: a wheel that does not run against the drive and does not slide sideways does not spin faster than the road goes
        // by under it. The engine's push is cut at what the tyre holds (above, with the friction circle), so the wheel is held to the speed
        // of the car, instead of spinning up on the part that it cannot put on the road. Without it, that part spins the wheel (a burnout).
        const float blocked = std::max(smoothstep(0.7f, 1.0f, brakeInput * P.brakeForce), handbrakeActsHere ? smoothstep(0.3f, 0.6f, handbrakeInput) : 0.0f);
        const bool slidingSideways = frictionLimit > 0 && (std::fabs(lateralForce) > kSidewaysSlideFraction * frictionLimit || wheel.slip > C.driftSlipDeadZone);
        const bool againstDrive = driveDemand * longitudinalSpeed < 0;
        const bool noBurnout = C.tractionControl > 0.5f && !slidingSideways && !againstDrive;
        float target = longitudinalSpeed / wheelRadii[i] * (1.0f - blocked);
        if (!noBurnout && std::fabs(driveDemand) > frictionLimit) {
            const float slipSpeed = (std::fabs(driveDemand) / std::max(frictionLimit, 1.0f) - 1.0f) * kBurnoutSlip * sizeScale_;
            target += (driveDemand > 0 ? 1.0f : -1.0f) * slipSpeed / wheelRadii[i];
        }
        target = clamp(target, -maxSpin, maxSpin);
        wheelSpeed_[i] += (target - wheelSpeed_[i]) * std::min(1.0f, stepSeconds * mix(kSpinFollow, 200.0f, blocked));
        wheel.spin += wheelSpeed_[i] * stepSeconds;
    }
    torqueWheels_ = totalTorque;
    // drift: 0 up to a slip angle that is just cornering, then rising to 1; never while slow
    drift_ = speed > C.driftMinSpeed ? clamp((largestSlipAngle - C.driftSlipDeadZone) / C.driftSlipRange, 0.0f, 1.0f) : 0.0f;

    // --- the whole car's forces, the damping of the spin (the car tumbles almost freely in the air), and the engine moves the body
    b3Body_ApplyForceToCenter(chassis, toBox(dragForce_ + downforce_ + thrustForce_), true);
    b3Body_SetAngularDamping(chassis, groundedCount == 0 ? P.angularDampingAir : P.angularDampingGround);
    b3World_Step(world, stepSeconds, kWorldSubSteps);
    readState();

    // --- the hull against the level: whether it touched, and the forces for the grey arrows
    const ContactSum hull = sumContacts(chassis, stepSeconds);
    hullTouched_ = hull.touching;
    readHullArrows(stepSeconds);
    torqueHull_ = hull.touching ? cross(hull.point - pos, hull.force) : Vec3f();
}

}  // namespace minigame01
