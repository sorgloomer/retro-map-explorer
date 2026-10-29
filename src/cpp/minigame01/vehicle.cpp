#include "minigame01/vehicle.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

Vec3f quatRotate(const Quat& q, const Vec3f& v) {
    Vec3f u(q.x, q.y, q.z);
    Vec3f t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Quat quatMul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quat quatNormalize(Quat q) {
    float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-9f) return {};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

void Vehicle::init(const CarModel& m) {
    for (int i = 0; i < 4; i++) wheelRest[i] = m.wheelPos[i];
    fallbackWheelRadius = m.wheelRadius;
    for (int i = 0; i < 4; i++) {
        meshWheelRadii[i] = m.meshWheelRadii[i];
        fileWheelRadii[i] = m.fileWheelRadii[i];
    }
    fileSpringK = m.fileSpringK;
    fileDamper = m.fileDamper;
    bmin = m.boundsMin;
    bmax = m.boundsMax;
    fileMass = m.mass;
    hasFileCom = m.hasCentreOfMass;
    fileCom = m.centreOfMass;
    com = Vec3f();  // nothing is placed yet: applyTuning has no position to keep
    applyTuning();
    reset(Vec3f(), 0);
}

// Works out the mass, the centre of mass, the inertia and the hull probes from the car's data and the tunable parameters.
// Called when the car is set up and whenever the parameters change; the car stays where it is (its origin does not move).
void Vehicle::applyTuning() {
    const Params& P = tuning();
    const Vec3f oldCom = com;
    // suspension and wheels: the car's own files when they say (and the parameters allow it), else the parameters / the meshes
    const bool fromFileSuspension = P.suspensionFromFile > 0.5f, fromFileRadius = P.wheelRadiusFromFile > 0.5f;
    springStiffness = fromFileSuspension && fileSpringK > 0 ? fileSpringK : P.springK;
    damping = fromFileSuspension && fileDamper > 0 ? fileDamper : P.damper;
    float radiusSum = 0;
    for (int i = 0; i < 4; i++) {
        wheelRadii[i] = fromFileRadius && fileWheelRadii[i] > 0 ? fileWheelRadii[i] : (meshWheelRadii[i] > 0 ? meshWheelRadii[i] : fallbackWheelRadius);
        radiusSum += wheelRadii[i];
    }
    wheelR = radiusSum * 0.25f;
    float frontZ = 0.5f * (wheelRest[0].z + wheelRest[1].z), rearZ = 0.5f * (wheelRest[2].z + wheelRest[3].z);
    float axleY = 0.25f * (wheelRest[0].y + wheelRest[1].y + wheelRest[2].y + wheelRest[3].y);
    if (P.centreOfMassFromFile > 0.5f && hasFileCom) {
        com = fileCom;  // the car's own files say where it is
    } else {
        // low, and towards the rear axle (so the rear tyres, which drive the car, carry more of the weight)
        com = Vec3f(0, axleY - wheelR + P.centreOfMassHeight, rearZ + P.centreOfMassFromRearAxle * (frontZ - rearZ));
    }
    float w = bmax.x - bmin.x, h = bmax.y - bmin.y, l = bmax.z - bmin.z;
    mass = P.mass > 0 ? P.mass : (fileMass > 0 ? fileMass : P.defaultMass);
    inertia = Vec3f(mass / 12.0f * (h * h + l * l), mass / 12.0f * (w * w + l * l), mass / 12.0f * (w * w + h * h) * P.rollInertiaFactor);
    probeR = P.probeRadius;
    // hull probes: a lattice of spheres inside the chassis bounds; the lowest row hovers above the road
    probes.clear();
    float groundY = axleY - wheelR;
    float yLow = groundY + 0.16f + probeR, yHigh = std::max(yLow + 0.1f, bmax.y - probeR);
    float xs = std::max(0.1f, 0.5f * w - probeR);
    for (float sx : {-xs, xs})
        for (int iz = 0; iz < 3; iz++)
            for (int iy = 0; iy < 3; iy++) {
                float z = bmin.z + probeR + (bmax.z - bmin.z - 2 * probeR) * 0.5f * iz;
                float y = yLow + (yHigh - yLow) * 0.5f * iy;
                probes.push_back(Vec3f(sx, y, z));
            }
    probes.push_back(Vec3f(0, yHigh, bmin.z + probeR));
    probes.push_back(Vec3f(0, yHigh, bmax.z - probeR));
    // pos is the centre of mass: when that moves within the car, move pos with it so that the car itself stays put
    updateAxes();
    pos += axes[0] * (com.x - oldCom.x) + axes[1] * (com.y - oldCom.y) + axes[2] * (com.z - oldCom.z);
}

void Vehicle::reset(const Vec3f& groundPos, float yaw) {
    float axleY = 0.25f * (wheelRest[0].y + wheelRest[1].y + wheelRest[2].y + wheelRest[3].y);
    // car origin height so that the wheels touch the road with the suspension at its rest length
    Vec3f origin = groundPos + Vec3f(0, wheelR - axleY + 0.02f, 0);
    q = quatNormalize({std::cos(yaw * 0.5f), 0, std::sin(yaw * 0.5f), 0});
    updateAxes();
    // pos is the centre of mass; the car origin is pos - R * com
    pos = origin + axes[0] * com.x + axes[1] * com.y + axes[2] * com.z;
    vel = angVel = Vec3f();
    steerAngle = 0;
    groundedCount = 0;
    drift_ = impact_ = 0;
    for (int i = 0; i < 4; i++) {
        wheels[i] = Wheel();
        wheels[i].centre = wheelRest[i];
    }
}

void Vehicle::updateAxes() {
    axes[0] = quatRotate(q, Vec3f(1, 0, 0));
    axes[1] = quatRotate(q, Vec3f(0, 1, 0));
    axes[2] = quatRotate(q, Vec3f(0, 0, 1));
}

Mat4 Vehicle::modelMatrix() const {
    Mat4 m = Mat4::identity();
    Vec3f origin = pos - (axes[0] * com.x + axes[1] * com.y + axes[2] * com.z);
    m.m[0] = axes[0].x; m.m[1] = axes[0].y; m.m[2] = axes[0].z;
    m.m[4] = axes[1].x; m.m[5] = axes[1].y; m.m[6] = axes[1].z;
    m.m[8] = axes[2].x; m.m[9] = axes[2].y; m.m[10] = axes[2].z;
    m.m[12] = origin.x;
    m.m[13] = origin.y;
    m.m[14] = origin.z;
    return m;
}

Vec3f Vehicle::invInertia(const Vec3f& t) const {
    Vec3f b(dot(t, axes[0]) / inertia.x, dot(t, axes[1]) / inertia.y, dot(t, axes[2]) / inertia.z);
    return axes[0] * b.x + axes[1] * b.y + axes[2] * b.z;
}

void Vehicle::step(float dt, const VehicleInput& in, const CollisionWorld& world) {
    const Params& P = tuning();
    dt = std::min(dt, P.vehicleMaxFrameSeconds);
    int n = std::max(1, (int)std::ceil(dt / P.maxPhysicsStep));
    float h = dt / (float)n;
    impact_ = 0;
    if (in.jump) vel += Vec3f(0, P.kangarooSpeed, 0);  // an impulse, once per press: straight up whatever the car's attitude
    for (int i = 0; i < n; i++) substep(h, in, world);
}

// One physics step of `stepSeconds`. Forces and torques are collected over the whole car (they act on the centre of mass, `pos`)
// and then integrated: first the four wheels (suspension, engine, brakes, tyre grip), then the chassis against the level.
void Vehicle::substep(float stepSeconds, const VehicleInput& in, const CollisionWorld& world) {
    const Params& P = tuning();
    updateAxes();
    const Vec3f rightAxis = axes[0], upAxis = axes[1], forwardAxis = axes[2];  // the car's own directions, in world space
    const float speed = length(vel);                     // m/s, in any direction
    const float forwardSpeed = dot(vel, forwardAxis);    // m/s along the nose: negative when rolling backwards

    // --- steering: full lock at a standstill, a fraction of it at speed; the wheels turn towards that angle smoothly
    const float maxSteerAngle = P.maxSteerAngle / (1.0f + std::pow(speed / P.steerFadeSpeed, P.steerFadeExponent));  // radians
    const float targetSteerAngle = clampf(in.steer, -1.0f, 1.0f) * maxSteerAngle;
    steerAngle += (targetSteerAngle - steerAngle) * std::min(1.0f, stepSeconds * P.steerResponse);

    // --- engine and brake intent
    float driveForce = 0;                  // N the engine pushes the car with, summed over the driven wheels (negative: reverse)
    float brakeInput = in.brake;           // 0..1
    const float handbrakeInput = in.handbrake;  // 0..1
    if (in.accel > P.minPedal) {
        // the engine's push is limited by its force at low speed and by its power (force * speed) at high speed
        const float maxDriveForce = std::min(P.engineForce, P.enginePower / std::max(std::fabs(forwardSpeed), P.powerMinSpeed));
        driveForce = in.accel * maxDriveForce;
        if (forwardSpeed < -1.0f) {  // accelerating while rolling backwards: brake first
            brakeInput = std::max(brakeInput, in.accel);
            driveForce = 0;
        }
    } else if (in.brake > P.minPedal && forwardSpeed < 1.0f) {  // the brake pedal doubles as reverse once the car has (almost) stopped
        driveForce = -in.brake * std::min(P.engineForce * P.reverseForceFraction,
                                          P.enginePower * P.reversePowerFraction / std::max(std::fabs(forwardSpeed), P.reversePowerMinSpeed));
        brakeInput = 0;
    }
    // the engine's push fades out over a speed range, so it has a soft top speed of its own (the afterburner is not affected)
    if (std::fabs(forwardSpeed) > P.taperStartSpeed && driveForce * forwardSpeed > 0)
        driveForce *= std::max(0.0f, 1.0f - (std::fabs(forwardSpeed) - P.taperStartSpeed) / P.taperLength);

    Vec3f totalForce(0, -P.gravity * mass, 0);  // N, on the centre of mass: starts with gravity
    Vec3f totalTorque;                         // N m, about the centre of mass
    // air drag against the velocity, and downforce pressing the car down onto its wheels
    totalForce += vel * (-P.drag * speed);
    totalForce += upAxis * (-P.downforce * speed * speed);
    if (in.afterburner) totalForce += forwardAxis * P.afterburnerForce;  // a rocket along the car's nose

    // --- the wheels
    groundedCount = 0;
    float largestSlipAngle = 0;  // radians, over the wheels on the ground: how sideways the car slides (see the drift measure below)
    for (int i = 0; i < 4; i++) {
        Wheel& wheel = wheels[i];
        const bool isFrontWheel = i < 2, isRearWheel = !isFrontWheel;  // wheels 0, 1 are the front ones, 2, 3 the rear (drive) ones
        // The suspension is a ray from its upper mount straight down (along the car's down axis): where it hits the road,
        // and how much of the suspension is left, tells how far the spring is compressed.
        const Vec3f wheelOffsetFromCentreOfMass = wheelRest[i] - com;  // in car space
        const Vec3f suspensionMount = pos + rightAxis * wheelOffsetFromCentreOfMass.x +
                                      upAxis * (wheelOffsetFromCentreOfMass.y + P.suspRest) +
                                      forwardAxis * wheelOffsetFromCentreOfMass.z;  // world position of the upper end of the suspension
        const float suspensionRayLength = P.suspMax + wheelRadii[i];  // fully extended suspension plus the wheel's radius
        float rayHitDistance;     // from the mount to the road
        Vec3f groundNormal;       // unit normal of the road where the ray hit
        wheel.grounded = false;
        wheel.load = 0;
        float suspensionLength = P.suspMax;  // from the mount to the wheel centre; the maximum when nothing is under the wheel
        if (world.raycast(suspensionMount, upAxis * -1.0f, suspensionRayLength, false, rayHitDistance, groundNormal)) {
            suspensionLength = std::max(P.suspMin, rayHitDistance - wheelRadii[i]);
            if (suspensionLength < P.suspMax) wheel.grounded = true;
        }
        if (wheel.grounded) {
            groundedCount++;
            const Vec3f contactPoint = suspensionMount - upAxis * rayHitDistance;
            const Vec3f contactOffset = contactPoint - pos;  // from the centre of mass to where the tyre touches the road
            const Vec3f mountVelocity = vel + cross(angVel, suspensionMount - pos);  // speed of the suspension's upper end

            // spring + damper (stiffer at the bump stop); a suspension cannot pull the car down, so never negative
            const float springCompression = P.springFree - suspensionLength;
            float suspensionForce = springStiffness * springCompression * (suspensionLength < P.suspMin + P.bumpStopZone ? P.bumpStopStiffness : 1.0f) +
                                    damping * (-dot(mountVelocity, upAxis));
            suspensionForce = std::max(0.0f, suspensionForce);
            wheel.load = suspensionForce;  // N: how hard the tyre is pressed on the road, which is what limits its grip
            const Vec3f suspensionForceVector = upAxis * suspensionForce;
            totalForce += suspensionForceVector;
            totalTorque += cross(contactOffset, suspensionForceVector);

            // The wheel's own axes on the road: where it rolls (the front wheels are turned by the steering, then the
            // direction is projected onto the road) and the direction across that.
            const float wheelSteerAngle = isFrontWheel ? steerAngle : 0.0f;
            const Vec3f wheelHeading = forwardAxis * std::cos(wheelSteerAngle) + rightAxis * std::sin(wheelSteerAngle);
            const Vec3f wheelForwardOnRoad = normalize(wheelHeading - groundNormal * dot(wheelHeading, groundNormal));
            const Vec3f wheelSidewaysOnRoad = normalize(cross(groundNormal, wheelForwardOnRoad));
            const Vec3f contactVelocity = vel + cross(angVel, contactOffset);  // how fast the road moves under the tyre
            const float longitudinalSpeed = dot(contactVelocity, wheelForwardOnRoad);  // along the wheel's rolling direction
            const float lateralSpeed = dot(contactVelocity, wheelSidewaysOnRoad);      // across it: this is skidding
            const float massPerWheel = mass * 0.25f;  // the part of the car's mass one wheel is responsible for

            // Force along the rolling direction: engine on the rear wheels (half each), brakes, rolling resistance.
            float longitudinalForce = 0;
            if (isRearWheel) longitudinalForce += driveForce * 0.5f;
            const float frictionLimit = P.grip * suspensionForce;  // N: the most the tyre can push with, in any direction
            float brakingForce = brakeInput * P.brakeForce * frictionLimit;
            if (isRearWheel) brakingForce = std::max(brakingForce, handbrakeInput * P.handbrakeBrake * frictionLimit);
            const float rollingDirection = longitudinalSpeed > 0 ? 1.0f : -1.0f;  // brakes and resistance act against the motion
            const float maxBrakingForce = std::fabs(longitudinalSpeed) * massPerWheel / stepSeconds;  // never brake past a standstill
            longitudinalForce -= rollingDirection * std::min(brakingForce, maxBrakingForce) * std::min(1.0f, std::fabs(longitudinalSpeed) * 2.0f);
            const float rollingResistanceForce = std::min(P.rollResist * suspensionForce, std::fabs(longitudinalSpeed) * massPerWheel / stepSeconds);
            longitudinalForce -= rollingDirection * rollingResistanceForce * (std::fabs(driveForce) > 1.0f ? P.rollResistWhileDriving : 1.0f);

            // Force across the wheel: the force that would stop the sideways slide within one step, limited by the grip
            // (the rear wheels lose grip with the handbrake); a tyre over the limit slides and keeps only part of it.
            const float handbrakeGripFactor = (isRearWheel && handbrakeInput > 0.05f) ? 1.0f - P.handbrakeGripLoss * handbrakeInput : 1.0f;
            const float lateralFrictionLimit = frictionLimit * handbrakeGripFactor;
            float lateralForce = -lateralSpeed * massPerWheel / stepSeconds * P.lateralStiffness;
            const bool isSliding = std::fabs(lateralForce) > lateralFrictionLimit;
            if (isSliding) lateralForce = (lateralForce > 0 ? 1.0f : -1.0f) * lateralFrictionLimit * P.slideGrip;
            // the friction circle: what the sideways force uses of the grip is not left for pushing along the wheel
            const float longitudinalFrictionLimit = std::sqrt(std::max(0.0f, frictionLimit * frictionLimit - lateralForce * lateralForce));
            longitudinalForce = clampf(longitudinalForce, -longitudinalFrictionLimit, longitudinalFrictionLimit);

            const Vec3f tyreForce = wheelForwardOnRoad * longitudinalForce + wheelSidewaysOnRoad * lateralForce;
            totalForce += tyreForce;
            const Vec3f tyreForceOffset = contactOffset + upAxis * P.rollCentre;  // acts a little above the road: less body roll
            totalTorque += cross(tyreForceOffset, tyreForce);

            wheel.lateralForce = lateralForce;
            wheel.lateralLimit = lateralFrictionLimit;
            wheel.sliding = isSliding;
            wheel.slip = std::atan2(std::fabs(lateralSpeed), std::fabs(longitudinalSpeed) + 1.0f);  // slip angle, radians (+1: no jump near standstill)
            largestSlipAngle = std::max(largestSlipAngle, wheel.slip);
            wheel.spin += (longitudinalSpeed / wheelRadii[i]) * stepSeconds;
            if (isFrontWheel) wheel.steer = wheelSteerAngle;
        } else {
            wheel.spin += (forwardSpeed / wheelRadii[i]) * stepSeconds * 0.5f;  // in the air: turns slowly with the car's speed
            wheel.slip = 0;
            wheel.lateralForce = wheel.lateralLimit = 0;
            wheel.sliding = false;
            if (isFrontWheel) wheel.steer = steerAngle;
        }
        wheel.centre = wheelRest[i] + Vec3f(0, P.suspRest - suspensionLength, 0);  // for drawing: lower when the suspension is extended
    }
    // drift: 0 up to a slip angle that is just cornering, then rising to 1; never while slow
    drift_ = speed > P.driftMinSpeed ? clampf((largestSlipAngle - P.driftSlipDeadZone) / P.driftSlipRange, 0.0f, 1.0f) : 0.0f;

    // damping of the spin: the car tumbles almost freely in the air
    if (groundedCount == 0) {
        angVel = angVel * (1.0f - P.airAngularDamping * stepSeconds);
    } else {
        angVel = angVel * (1.0f - P.groundAngularDamping * stepSeconds);
    }

    // --- integrate: forces change the velocity, torques the spin, those move and turn the car
    vel += totalForce * (stepSeconds / mass);
    angVel += invInertia(totalTorque) * stepSeconds;
    const float speedAfterStep = length(vel);
    if (speedAfterStep > P.maxSpeed) vel = vel * (P.maxSpeed / speedAfterStep);
    pos += vel * stepSeconds;
    // the orientation turns with the spin: dq/dt = 0.5 * (0, angVel) * q
    const Quat orientationRate = quatMul({0, angVel.x, angVel.y, angVel.z}, q);
    q = quatNormalize({q.w + 0.5f * stepSeconds * orientationRate.w, q.x + 0.5f * stepSeconds * orientationRate.x,
                       q.y + 0.5f * stepSeconds * orientationRate.y, q.z + 0.5f * stepSeconds * orientationRate.z});
    updateAxes();

    // --- hull against the level: walls, ceilings, the road when the chassis bottoms out. Each probe sphere that overlaps
    // the level gets an impulse that stops it moving into the surface, a friction impulse along the surface, and is pushed out.
    const Vec3f carOrigin = pos - (axes[0] * com.x + axes[1] * com.y + axes[2] * com.z);
    for (const Vec3f& probeInCarSpace : probes) {
        const Vec3f probeCentre = carOrigin + axes[0] * probeInCarSpace.x + axes[1] * probeInCarSpace.y + axes[2] * probeInCarSpace.z;
        Vec3f contactNormal;   // from the level towards the probe
        float penetrationDepth;
        if (!world.sphereContact(probeCentre, probeR, contactNormal, penetrationDepth)) continue;
        const Vec3f probeOffset = probeCentre - pos;  // from the centre of mass to the probe
        Vec3f probeVelocity = vel + cross(angVel, probeOffset);
        const float normalSpeed = dot(probeVelocity, contactNormal);  // negative: the probe moves into the surface
        const bool isFloor = contactNormal.y > P.floorNormalY;
        if (normalSpeed < 0) {
            // normal impulse: the usual rigid body formula, with the car's mass and rotation taken into account
            const Vec3f normalRotationResponse = invInertia(cross(probeOffset, contactNormal));
            const float normalDenominator = 1.0f / mass + dot(contactNormal, cross(normalRotationResponse, probeOffset));
            const float restitution = isFloor ? 0.0f : P.wallBounce;
            const float normalImpulse = -(1.0f + restitution) * normalSpeed / normalDenominator;
            vel += contactNormal * (normalImpulse / mass);
            angVel += invInertia(cross(probeOffset, contactNormal * normalImpulse));
            impact_ = std::max(impact_, -normalSpeed);
            // Coulomb friction along the surface: stops the sliding, but not by more than a part of the normal impulse
            probeVelocity = vel + cross(angVel, probeOffset);
            const Vec3f tangentialVelocity = probeVelocity - contactNormal * dot(probeVelocity, contactNormal);
            const float tangentialSpeed = length(tangentialVelocity);
            if (tangentialSpeed > 1e-4f) {
                const Vec3f tangentDirection = tangentialVelocity * (1.0f / tangentialSpeed);
                const Vec3f tangentRotationResponse = invInertia(cross(probeOffset, tangentDirection));
                const float tangentDenominator = 1.0f / mass + dot(tangentDirection, cross(tangentRotationResponse, probeOffset));
                const float frictionImpulse = std::min(tangentialSpeed / tangentDenominator, (isFloor ? P.floorFriction : P.wallFriction) * normalImpulse);
                vel += tangentDirection * (-frictionImpulse / mass);
                angVel += invInertia(cross(probeOffset, tangentDirection * -frictionImpulse));
            }
        }
        pos += contactNormal * std::min(penetrationDepth * P.hullPushOut, P.hullMaxPushOut);  // out of the surface
    }
}

}  // namespace minigame01
