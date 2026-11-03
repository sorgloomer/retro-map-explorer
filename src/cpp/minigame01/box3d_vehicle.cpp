#ifndef RME_PHYSICS_LEGACY  // (the build says --legacy-physics: the car runs on vehicle.cpp, and Box3D is not part of the build)
#include "minigame01/box3d_vehicle.h"
#include <algorithm>
#include <cmath>
#include "math/functions.h"

namespace minigame01 {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kWheelMassFraction = 0.03f;  // of the mass of the car, for every wheel
constexpr int kMaxContacts = 16;
constexpr int kMaxHullVertices = 64;   // of the hull of the chassis
// The engine applies the restitution of the hull to every surface (the car bounced on landing): impacts slower than this do not bounce.
constexpr float kBounceMinSpeed = 3.0f;         // m/s for a car of 4.3 m: the impact speed from which the hull bounces
constexpr float kBurnoutSlip = 25.0f;           // m/s of slip (for a car of 4.3 m) of a driven wheel for every time that the engine asks for more than the tyre holds
constexpr float kSidewaysSlideFraction = 0.9f;  // a tyre that uses this part of its grip sideways is sliding (traction control lets go)
constexpr float kSpinFollow = 25.0f;           // 1/s: how fast a wheel on the road comes to the speed the road gives it

inline b3Vec3 toBox(const Vec3f& v) { return {v.x, v.y, v.z}; }
inline Vec3f fromBox(const b3Vec3& v) { return Vec3f(v.x, v.y, v.z); }

// The faces of a hull (convex polygons, as half-edges) as triangles: every face a fan from its first corner.
void hullTriangles(const b3HullData* hull, std::vector<Vec3f>& vertices, std::vector<uint32_t>& indices) {
    const b3Vec3* points = b3GetHullPoints(hull);
    const b3HullHalfEdge* edges = b3GetHullEdges(hull);
    const b3HullFace* faces = b3GetHullFaces(hull);
    vertices.clear();
    indices.clear();
    for (int i = 0; i < hull->vertexCount; i++) vertices.push_back(fromBox(points[i]));
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

// The frame of a wheel joint in the chassis and in the wheel: its x axis is the one of the suspension (the car's up), its z axis the axle
// (the car's right), as the wheel joint wants them.
b3Quat wheelJointFrame() {
    b3Matrix3 m;
    m.cx = {0.0f, 1.0f, 0.0f};
    m.cy = {0.0f, 0.0f, 1.0f};
    m.cz = {1.0f, 0.0f, 0.0f};
    return b3MakeQuatFromMatrix(&m);
}

// What the level does to one body, from the contacts of the last solver step: the force (N), where it acts (the points weighted by their
// normal force), the normal force and the average normal.
using ContactSum = Box3dVehicle::ContactSum;

// Adds the contacts of one solver step to the sum over the physics step (weight: its part of it).
void accumulateContact(ContactSum& total, const ContactSum& step, float weight) {
    total.force += step.force * weight;
    total.frictionForce += step.frictionForce * weight;
    total.normalForce += step.normalForce * weight;
    if (!step.touching) return;
    total.touching = true;
    total.point = step.point;
    total.normal = step.normal;
}

ContactSum sumContacts(b3BodyId body, float stepSeconds, float subStepSeconds) {
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
                normalImpulse += point.totalNormalImpulse;  // (what the contact pushed with over the step: the impulse of the last solver step can be none)
                const Vec3f at = centre + fromBox(isB ? point.anchorB : point.anchorA);
                pointSum += at * point.totalNormalImpulse;
                weightSum += point.totalNormalImpulse;
            }
            if (normalImpulse <= 0) continue;
            sum.touching = true;
            sum.force += normal * (normalImpulse / stepSeconds);
            sum.frictionForce += fromBox(manifold.frictionImpulse) * side * (1.0f / subStepSeconds);
            sum.normalForce += normalImpulse / stepSeconds;
            normalSum += normal * normalImpulse;
        }
    }
    if (weightSum > 0) sum.point = pointSum * (1.0f / weightSum);
    if (sum.touching) sum.normal = normalize(normalSum);
    return sum;
}

}  // namespace

void Box3dVehicle::destroyBodies() {
    if (!B3_IS_NULL(world) && b3World_IsValid(world)) {
        for (b3BodyId& body : wheelBody) if (!B3_IS_NULL(body)) b3DestroyBody(body);  // (its joint goes with it)
        if (!B3_IS_NULL(chassis)) b3DestroyBody(chassis);
    }
    chassis = b3_nullBodyId;
    for (b3BodyId& body : wheelBody) body = b3_nullBodyId;
}

void Box3dVehicle::init(const CarModel& m) {
    for (int i = 0; i < kWheelCount; i++) wheelRest[i] = m.wheelPos[i];
    bmin = m.boundsMin;
    bmax = m.boundsMax;
    hasHullBox = m.hasHullBox;
    hullBoxMin = m.hullBoxMin;
    hullBoxMax = m.hullBoxMax;
    hullPoints_ = m.hullPoints;
    modelHullIndices_ = m.hullTriangleIndices;
    centreOfMass = Vec3f();
    applyTuning();
    reset(Vec3f(), 0);
}

// Takes the values of the car from the settings and makes its bodies and joints from them. Called when the car is set up and whenever the
// settings change: the car stays where it is (the new bodies get the pose and the speed of the old ones).
void Box3dVehicle::applyTuning() {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    toy_ = C.toy > 0.5f;
    sizeScale_ = C.sizeScale;
    gravity_ = C.gravity;
    engineForce_ = C.engineForce;
    enginePower_ = C.enginePower;
    grip_ = C.grip;  // the friction coefficient of the tyres: the same in every direction (the engine's friction holds the car sideways, and the engine and the brakes push up to it)
    dragCoefficient_ = C.drag;
    afterburnerForce_ = C.afterburnerForce;
    kangarooSpeed_ = C.kangarooSpeed;
    mass = C.mass;
    suspensionLengthMax_ = C.suspensionLengthMax;
    suspensionLengthAtRest_ = C.suspensionLengthAtRest;
    suspensionLengthInLoadedModel_ = C.suspensionLengthInLoadedModel;
    wheelRadii[0] = C.wheelRadius0;
    wheelRadii[1] = C.wheelRadius1;
    wheelRadii[2] = C.wheelRadius2;
    wheelRadii[3] = C.wheelRadius3;
    centreOfMass = Vec3f(C.centreOfMassX, C.centreOfMassY, C.centreOfMassZ);
    wheelMass = mass * kWheelMassFraction;
    const float chassisMass = mass - kWheelCount * wheelMass;
    const float w = bmax.x - bmin.x, h = bmax.y - bmin.y, l = bmax.z - bmin.z;
    inertia = Vec3f(chassisMass / 12.0f * (h * h + l * l), chassisMass / 12.0f * (w * w + l * l), chassisMass / 12.0f * (w * w + h * h) * P.rollInertiaFactor);
    const float springK = std::max(1.0f, C.suspensionSpringK);
    // the length of the spring in isolation: what the chassis' share of the weight compresses to the rest length of the suspension
    suspensionSpringNaturalLength_ = suspensionLengthAtRest_ + chassisMass * gravity_ / (kWheelCount * springK);
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

    // the chassis: the hull of the car's files (else the bounds of its body) as a box, with the mass and the inertia of the car
    {
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
        data.mass = chassisMass;
        data.center = toBox(centreOfMass);
        data.inertia.cx = {inertia.x, 0, 0};
        data.inertia.cy = {0, inertia.y, 0};
        data.inertia.cz = {0, 0, inertia.z};
        b3Body_SetMassData(chassis, data);
        b3Body_SetLinearVelocity(chassis, linear);
        b3Body_SetAngularVelocity(chassis, angular);
    }

    // the wheels: a sphere each, on a wheel joint. Joint frames: the suspension's x axis is up, the axle is z, the spring's rest is the
    // place of the wheel at the natural length of the spring (the translation along the suspension is then how far the spring is pressed).
    const b3Quat frame = wheelJointFrame();
    for (int i = 0; i < kWheelCount; i++) {
        const Vec3f atRest = wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - suspensionLengthAtRest_, 0);
        b3BodyDef def = b3DefaultBodyDef();
        def.type = b3_dynamicBody;
        def.position = b3TransformPoint(pose, toBox(atRest));
        def.rotation = pose.q;
        def.enableSleep = false;
        def.allowFastRotation = true;  // a wheel turns many times a second
        wheelBody[i] = b3CreateBody(world, &def);
        b3ShapeDef shape = b3DefaultShapeDef();
        const float volume = 4.0f / 3.0f * kPi * wheelRadii[i] * wheelRadii[i] * wheelRadii[i];
        shape.density = wheelMass / volume;
        shape.baseMaterial.friction = grip_;             // against the level: this times the surface's multiplier
        shape.baseMaterial.restitution = 0.0f;
        shape.baseMaterial.rollingResistance = P.rollResist;
        const b3Sphere sphere = {{0, 0, 0}, wheelRadii[i]};
        wheelShape[i] = b3CreateSphereShape(wheelBody[i], &shape, &sphere);
        b3Body_SetLinearVelocity(wheelBody[i], linear);
        b3Body_SetAngularVelocity(wheelBody[i], angular);

        // the spring of the joint is a soft constraint on the effective mass of the joint: the stiffness is that times the square of its angular
        // frequency, so the frequency comes from the stiffness we want and the mass that the wheel and its corner of the chassis move together
        const Vec3f lever = wheelRest[i] - centreOfMass;
        const float inverseSprung = 1.0f / chassisMass + lever.z * lever.z / inertia.x + lever.x * lever.x / inertia.z;
        const float effective = 1.0f / (inverseSprung + 1.0f / wheelMass);
        const float hertz = std::sqrt(springK / effective) / (2.0f * kPi);
        const float dampingRatio = C.suspensionDamperCoefficient / (2.0f * std::sqrt(springK * effective));

        const bool front = i < 2;
        b3WheelJointDef joint = b3DefaultWheelJointDef();
        joint.base.bodyIdA = chassis;
        joint.base.bodyIdB = wheelBody[i];
        joint.base.localFrameA.p = toBox(wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - suspensionSpringNaturalLength_, 0));
        joint.base.localFrameA.q = frame;
        joint.base.localFrameB.p = {0, 0, 0};
        joint.base.localFrameB.q = frame;
        joint.base.collideConnected = false;
        joint.enableSuspensionSpring = true;
        joint.suspensionHertz = hertz;
        joint.suspensionDampingRatio = dampingRatio;
        joint.enableSuspensionLimit = true;
        joint.lowerSuspensionLimit = suspensionSpringNaturalLength_ - suspensionLengthMax_;  // (translation: how far the spring is pressed)
        joint.upperSuspensionLimit = suspensionSpringNaturalLength_;                         // a spring of length 0: the stop
        joint.enableSpinMotor = false;   // (the wheels turn freely: the engine and the brakes push the wheels as forces, see substep)
        joint.enableSteering = front;
        joint.steeringHertz = 10.0f;
        joint.steeringDampingRatio = 0.7f;
        joint.maxSteeringTorque = 2.0f * grip_ * (mass * gravity_ / kWheelCount) * wheelRadii[i];  // twice what turns a loaded tyre on the road
        joint.enableSteeringLimit = front;
        joint.lowerSteeringLimit = -1.5f * C.steerAngleMax;
        joint.upperSteeringLimit = 1.5f * C.steerAngleMax;
        wheelJoint[i] = b3CreateWheelJoint(world, &joint);
    }
    readState();
}

void Box3dVehicle::reset(const Vec3f& groundPos, float yaw) {
    if (B3_IS_NULL(chassis)) return;
    const b3Quat q = b3MakeQuatFromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
    // the car's origin so that the wheels touch the road with the suspension at its rest length
    float lift = -1e30f;
    for (int i = 0; i < kWheelCount; i++) lift = std::max(lift, wheelRadii[i] - (wheelRest[i].y + suspensionLengthInLoadedModel_ - suspensionLengthAtRest_));
    const b3Vec3 origin = toBox(groundPos + Vec3f(0, lift + 0.02f * sizeScale_, 0));
    const b3Transform pose = {origin, q};
    b3Body_SetTransform(chassis, origin, q);
    b3Body_SetLinearVelocity(chassis, {0, 0, 0});
    b3Body_SetAngularVelocity(chassis, {0, 0, 0});
    for (int i = 0; i < kWheelCount; i++) {
        const Vec3f atRest = wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - suspensionLengthAtRest_, 0);
        b3Body_SetTransform(wheelBody[i], b3TransformPoint(pose, toBox(atRest)), q);
        b3Body_SetLinearVelocity(wheelBody[i], {0, 0, 0});
        b3Body_SetAngularVelocity(wheelBody[i], {0, 0, 0});
        wheels[i] = Wheel();
        wheels[i].centre = atRest;
        wheelSpeed_[i] = 0;
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
    const b3Quat delta = b3MulQuat(turned, b3Conjugate(now.q));                      // what the car turns by, in the world
    const b3Vec3 centre = b3Body_GetWorldCenter(chassis);
    const b3Vec3 newCentre = {centre.x, centre.y + metres, centre.z};
    b3Body_SetTransform(chassis, b3Sub(newCentre, b3RotateVector(turned, toBox(centreOfMass))), turned);
    for (int i = 0; i < kWheelCount; i++) {
        const b3WorldTransform w = b3Body_GetTransform(wheelBody[i]);
        b3Body_SetTransform(wheelBody[i], b3Add(newCentre, b3RotateVector(delta, b3Sub(w.p, centre))), b3MulQuat(delta, w.q));
    }
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
    float massSum = b3Body_GetMass(chassis);
    Vec3f weighted = pos * massSum;
    for (int i = 0; i < kWheelCount; i++) {
        const float wheelMassNow = b3Body_GetMass(wheelBody[i]);
        weighted += fromBox(b3Body_GetWorldCenter(wheelBody[i])) * wheelMassNow;
        massSum += wheelMassNow;
    }
    carCentre_ = weighted * (1.0f / massSum);
    vel = fromBox(b3Body_GetLinearVelocity(chassis));
    angVel = fromBox(b3Body_GetAngularVelocity(chassis));
    for (int i = 0; i < kWheelCount; i++) {
        wheels[i].centre = fromBox(b3Body_GetLocalPoint(chassis, b3Body_GetPosition(wheelBody[i])));
        wheels[i].steer = i < 2 ? b3WheelJoint_GetSteeringAngle(wheelJoint[i]) : 0.0f;
    }
}

// The contacts of the last solver step: what the level pushes every tyre and the hull with, where, and the numbers of the debug display.
// The contacts of the physics step that has been made (wheelContact_, summed over its solver steps; the hull's are the last solver step's):
// what the level pushes every tyre and the hull with, where, and the numbers of the debug display. `solverSeconds`: the length of a solver step.
void Box3dVehicle::readContacts(float solverSeconds) {
    const CarSettings& C = carSettings();
    groundedCount = 0;
    float largestSlipAngle = 0;
    torqueWheels_ = torqueHull_ = Vec3f();
    for (int i = 0; i < kWheelCount; i++) {
        Wheel& wheel = wheels[i];
        const ContactSum& contact = wheelContact_[i];
        const b3WorldTransform wheelTransform = b3Body_GetTransform(wheelBody[i]);
        wheel.grounded = contact.touching;
        wheel.contactPoint = contact.point;
        wheel.suspensionForceOrigin = fromBox(wheelTransform.p);
        if (!contact.touching) {
            wheel.tyreForce = wheel.force = Vec3f();
            wheel.tyreForceOrigin = fromBox(wheelTransform.p);
            wheel.load = 0;
            wheel.slip = 0;
            wheel.lateralForce = wheel.lateralLimit = wheel.longitudinalForce = wheel.frictionLimit = 0;
            wheel.sliding = false;
            continue;
        }
        groundedCount++;
        // the wheel's own axes on the road: along its heading (the axle turned a quarter), and across
        const Vec3f axle = fromBox(b3RotateVector(wheelTransform.q, {1.0f, 0.0f, 0.0f}));
        const Vec3f heading = normalize(cross(axle, axes[1]));
        const Vec3f forwardOnRoad = normalize(heading - contact.normal * dot(heading, contact.normal));
        const Vec3f sidewaysOnRoad = normalize(cross(contact.normal, forwardOnRoad));
        // The tyre force, along the road: the friction of the tyre (the friction impulse of its contacts), and what the engine and the brakes
        // push with. (What presses the tyre on the road is the suspension's force, below: the two arrows do not both have it.)
        const Vec3f friction = contact.frictionForce;
        rollDirection_[i] = forwardOnRoad;
        wheel.tyreForce = friction + wheelPush_[i];
        wheel.longitudinalForce = dot(friction + wheelPush_[i], forwardOnRoad);
        wheel.lateralForce = dot(friction, sidewaysOnRoad);
        // The forces act at the contact point: the friction, what the engine and the brakes push with (it is applied there, see substep) and
        // what presses the tyre on the road.
        wheel.tyreForceOrigin = contact.point;
        wheel.force = axes[1] * contact.normalForce;
        wheel.suspensionForceOrigin = contact.point;
        // the surface under the tyre: the multiplier of its friction (a ray from just above the contact point down into the road); the
        // engine's own friction has it already (the friction callback of the level), the pushes of the engine and the brakes go by it here
        roadFriction_[i] = 1.0f;
        if (level_) {
            const float reach = 0.25f * wheelRadii[i];
            const b3RayResult hit = b3World_CastRayClosest(world, toBox(contact.point + contact.normal * (0.5f * reach)), toBox(contact.normal * -reach), b3DefaultQueryFilter());
            if (hit.hit) roadFriction_[i] = level_->surfaceFriction(hit.userMaterialId);
        }
        wheel.load = contact.normalForce;
        wheel.frictionLimit = C.grip * roadFriction_[i] * contact.normalForce;
        wheel.lateralLimit = wheel.frictionLimit;
        const float tangential = std::sqrt(wheel.longitudinalForce * wheel.longitudinalForce + wheel.lateralForce * wheel.lateralForce);
        wheel.sliding = wheel.frictionLimit > 0 && tangential > 0.98f * wheel.frictionLimit;
        const Vec3f contactVelocity = fromBox(b3Body_GetWorldPointVelocity(wheelBody[i], toBox(contact.point)));
        wheel.slip = std::atan2(std::fabs(dot(contactVelocity, sidewaysOnRoad)), std::fabs(dot(contactVelocity, forwardOnRoad)) + 1.0f);
        largestSlipAngle = std::max(largestSlipAngle, wheel.slip);
        torqueWheels_ += cross(wheel.tyreForceOrigin - pos, wheel.tyreForce) + cross(wheel.suspensionForceOrigin - pos, wheel.force);
    }
    const ContactSum hull = sumContacts(chassis, solverSeconds, solverSeconds);
    hullTouched_ = hull.touching;
    readHullArrows(solverSeconds);
    if (hull.touching) torqueHull_ = cross(hull.point - pos, hull.force + hull.frictionForce);
    drift_ = length(vel) > C.driftMinSpeed ? clamp((largestSlipAngle - C.driftSlipDeadZone) / C.driftSlipRange, 0.0f, 1.0f) : 0.0f;
}

void Box3dVehicle::step(float dt, const VehicleInput& in) {
    if (B3_IS_NULL(chassis)) return;
    const Params& P = tuning();
    dt = std::min(dt, P.vehicleMaxFrameSeconds);
    const int n = std::max(1, (int)std::ceil(dt / P.maxPhysicsStep));
    const float h = dt / (float)n;
    impact_ = 0;
    if (in.jump) {  // an impulse, once per press: straight up whatever the car's attitude
        for (b3BodyId body : {chassis, wheelBody[0], wheelBody[1], wheelBody[2], wheelBody[3]}) {
            const b3Vec3 v = b3Body_GetLinearVelocity(body);
            b3Body_SetLinearVelocity(body, {v.x, v.y + kangarooSpeed_, v.z});
        }
    }
    for (int i = 0; i < n; i++) substep(h, in);
}

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
    for (int i = 0; i < 2; i++) b3WheelJoint_SetTargetSteeringAngle(wheelJoint[i], steerAngle);

    // --- the tyres' forces along their heading: the engine on the rear wheels (half each), the brakes (and the handbrake) on the ones they act
    // on. They are forces on the wheels (the friction of the engine holds the car sideways, and the same grip limits both): the tyre pushes
    // with at most grip * its load (and the multiplier of the surface). They act at the contact point, on the car.
    // Every wheel decides for itself what the pedals mean, by how it moves along the road: the accelerator brakes a wheel that runs backwards
    // and drives it otherwise; the brake pedal brakes a wheel that runs forwards, and drives a driven one backwards otherwise. A wheel in the
    // air has no brake to use: it spins up (the handbrake does not block it either).
    constexpr float kMinPowerSpeed = 1e-3f;
    const float handbrakeInput = in.handbrake;
    for (int i = 0; i < kWheelCount; i++) {
        const bool rear = i >= 2;
        const bool touching = wheels[i].grounded;
        const float rolling = touching ? dot(fromBox(b3Body_GetLinearVelocity(wheelBody[i])), rollDirection_[i]) : 0.0f;   // m/s along the heading
        const float surfaceSpeed = touching ? rolling : wheelSpeed_[i] * wheelRadii[i];   // how fast the tyre runs along the road, or would
        float driveDemand = 0;     // N at the tyre (negative: reverse)
        float brakeInput = 0;
        if (in.accel > P.controlMinPedal) {
            brakeInput = in.brake;
            if (touching && rolling < -1.0f) brakeInput = std::max(brakeInput, in.accel);
            else if (rear) driveDemand = 0.5f * in.accel * std::min(engineForce_, enginePower_ / std::max(std::fabs(surfaceSpeed), kMinPowerSpeed));
        } else if (in.brake > P.controlMinPedal) {
            if (touching && rolling >= 1.0f) brakeInput = in.brake;
            else if (rear) driveDemand = -0.5f * in.brake * std::min(engineForce_ * C.reverseForceFraction,
                                                                   enginePower_ * C.reversePowerFraction / std::max(std::fabs(surfaceSpeed), kMinPowerSpeed));
        }
        if (!touching) brakeInput = 0;
        if (std::fabs(surfaceSpeed) > C.engineTaperStartSpeed && driveDemand * surfaceSpeed > 0)
            driveDemand *= std::max(0.0f, 1.0f - (std::fabs(surfaceSpeed) - C.engineTaperStartSpeed) / C.engineTaperLength);
        const bool handbrakeActsHere = rear || C.handbrakeAllWheels > 0.5f;
        // the handbrake takes part of the sideways grip of the wheels it acts on
        const float gripFactor = (handbrakeActsHere && handbrakeInput > 0.05f) ? 1.0f - C.handbrakeGripLoss * handbrakeInput : 1.0f;
        b3Shape_SetFriction(wheelShape[i], grip_ * gripFactor);
        wheelPush_[i] = Vec3f();
        // How the wheel turns (the wheels of the engine turn freely: this is for the look of it, and goes by what the engine and the brakes
        // ask for): the engine turns it up to the speed the engine has at most, a brake blocks it (the handbrake too), on the road it follows
        // the road, and spins faster when the engine asks for more than the tyre holds.
        const float blocked = !touching ? 0.0f : std::max(smoothstep(0.7f, 1.0f, brakeInput * P.brakeForce), handbrakeActsHere ? smoothstep(0.3f, 0.6f, handbrakeInput) : 0.0f);
        const float maxSpin = (C.engineTaperStartSpeed + C.engineTaperLength) / wheelRadii[i];
        if (!touching) {
            const float inertia = 0.4f * wheelMass * wheelRadii[i] * wheelRadii[i];
            float spinSpeed = wheelSpeed_[i] + driveDemand * wheelRadii[i] / inertia * stepSeconds;
            spinSpeed = clamp(spinSpeed, -maxSpin, maxSpin) * (1.0f - blocked * std::min(1.0f, stepSeconds * 40.0f)) * (1.0f - 0.3f * stepSeconds);
            wheelSpeed_[i] = spinSpeed;
            continue;
        }
        const float limit = C.grip * roadFriction_[i] * wheels[i].load;   // the surface counts: on ice the tyre pushes with nothing
        // Traction control: a wheel that does not run against the drive and does not slide sideways does not spin faster than the road goes
        // by under it. The engine's push is cut at what the tyre holds (below, with the friction circle), so the wheel is held to the speed
        // of the car, instead of spinning up on the part that it cannot put on the road. Without it, that part spins the wheel (a burnout).
        const bool slidingSideways = limit > 0 && (std::fabs(wheels[i].lateralForce) > kSidewaysSlideFraction * limit || wheels[i].slip > C.driftSlipDeadZone);
        const bool againstDrive = driveDemand * rolling < 0;
        const bool noBurnout = C.tractionControl > 0.5f && !slidingSideways && !againstDrive;
        {
            float target = rolling / wheelRadii[i] * (1.0f - blocked);
            if (!noBurnout && std::fabs(driveDemand) > limit) {
                const float slipSpeed = (std::fabs(driveDemand) / std::max(limit, 1.0f) - 1.0f) * kBurnoutSlip * sizeScale_;
                target += (driveDemand > 0 ? 1.0f : -1.0f) * slipSpeed / wheelRadii[i];
            }
            target = clamp(target, -maxSpin, maxSpin);
            wheelSpeed_[i] += (target - wheelSpeed_[i]) * std::min(1.0f, stepSeconds * mix(kSpinFollow, 200.0f, blocked));
        }
        float force = driveDemand;
        float brake = brakeInput * P.brakeForce * limit;
        if (handbrakeActsHere) brake = std::max(brake, handbrakeInput * C.handbrakeBrake * limit);
        if (brake > 0) {   // against the motion, never past a standstill
            const float stopForce = std::fabs(rolling) * (mass / kWheelCount) / stepSeconds * 0.5f;
            force -= (rolling > 0 ? 1.0f : -1.0f) * std::min(brake, stopForce) * std::min(1.0f, std::fabs(rolling) * 2.0f);
        }
        // the friction circle: what the tyre uses of its grip sideways (the friction the contacts reported in the last step) is not left to push along
        const float longitudinalLimit = std::sqrt(std::max(0.0f, limit * limit - wheels[i].lateralForce * wheels[i].lateralForce));
        force = clamp(force, -longitudinalLimit, longitudinalLimit);
        wheelPush_[i] = rollDirection_[i] * force;
    }

    // --- forces on the whole car at its centre of mass: air drag, downforce, the rockets
    dragForce_ = vel * (-dragCoefficient_ * speed);
    const float downforceMagnitude = C.downforceRate * (std::sqrt(speed * speed + C.downforceKnee * C.downforceKnee) - C.downforceKnee);
    downforce_ = upAxis * -downforceMagnitude;
    thrustForce_ = Vec3f();
    if (in.afterburner) thrustForce_ += forwardAxis * afterburnerForce_;
    if (in.sideThrust) thrustForce_ += rightAxis * afterburnerForce_;
    gravityForce_ = Vec3f(0, -gravity() * mass, 0);

    // The world is stepped in kSolverSteps steps of one solver step each, not in one step of several: the friction impulse that the engine keeps
    // of a contact is the one of the last solver step of a step (the accumulated friction state after it), and over a step of several the
    // contacts of our tyres have none left in it; over one solver step it is the friction of that step. The forces last for one world step.
    const float solverSeconds = stepSeconds / (float)kSolverSteps;
    for (int i = 0; i < kWheelCount; i++) wheelContact_[i] = ContactSum();
    for (int step = 0; step < kSolverSteps; step++) {
        // the drag, the downforce and the rockets through the centre of mass of the whole car (the wheels have weight too: through the
        // chassis' own, the rockets would turn the car), the pushes of the engine and the brakes at the contact points, on the car
        b3Body_ApplyForce(chassis, toBox(dragForce_ + downforce_ + thrustForce_), toBox(carCentre_), true);
        for (int i = 0; i < kWheelCount; i++)
            if (wheels[i].grounded) b3Body_ApplyForce(chassis, toBox(wheelPush_[i]), toBox(wheels[i].contactPoint), true);
        b3World_Step(world, solverSeconds, 1);
        for (int i = 0; i < kWheelCount; i++) accumulateContact(wheelContact_[i], sumContacts(wheelBody[i], solverSeconds, solverSeconds), 1.0f / (float)kSolverSteps);
    }
    readState();
    readContacts(solverSeconds);
    for (int i = 0; i < kWheelCount; i++) wheels[i].spin += wheelSpeed_[i] * stepSeconds;
}

}  // namespace minigame01
#endif  // RME_PHYSICS_LEGACY
