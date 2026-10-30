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
    bmin = m.boundsMin;
    bmax = m.boundsMax;
    hasHullBox = m.hasHullBox;
    hullBoxMin = m.hullBoxMin;
    hullBoxMax = m.hullBoxMax;
    centreOfMass = Vec3f();  // nothing is placed yet: applyTuning has no position to keep (the settings are in effect before the car is set up)
    applyTuning();
    reset(Vec3f(), 0);
}

// Takes the values of the car from the settings (computed by the configuration manager, see params.h) and works out the inertia and
// the hull probes. Called when the car is set up and whenever the settings change; the car stays where it is (its origin does not move).
void Vehicle::applyTuning() {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    const Vec3f previousCentreOfMass = centreOfMass;
    toy_ = C.toy > 0.5f;
    sizeScale_ = C.sizeScale;
    gravity_ = P.gravity;
    engineForce_ = C.engineForce;
    enginePower_ = C.enginePower;
    grip_ = C.grip;
    dragCoefficient_ = C.drag;
    afterburnerForce_ = C.afterburnerForce;
    kangarooSpeed_ = C.kangarooSpeed;
    mass = C.mass;
    suspMax_ = C.suspMax;
    suspMin_ = C.suspMin;
    suspRest_ = C.suspRest;
    bumpZone_ = C.bumpStopZone;
    springFree_ = C.springFree;
    springStiffness = C.springK;
    damping = C.damper;
    wheelRadii[0] = C.wheelRadius0;
    wheelRadii[1] = C.wheelRadius1;
    wheelRadii[2] = C.wheelRadius2;
    wheelRadii[3] = C.wheelRadius3;
    wheelR = 0.25f * (wheelRadii[0] + wheelRadii[1] + wheelRadii[2] + wheelRadii[3]);
    centreOfMass = Vec3f(C.centreOfMassX, C.centreOfMassY, C.centreOfMassZ);
    const float w = bmax.x - bmin.x, h = bmax.y - bmin.y, l = bmax.z - bmin.z;
    inertia = Vec3f(mass / 12.0f * (h * h + l * l), mass / 12.0f * (w * w + l * l), mass / 12.0f * (w * w + h * h) * P.rollInertiaFactor);
    probeR = P.probeRadius;
    // Hull probes: one layer of a grid of 3 x 5 spheres (across, along), the bottom of every sphere at the bottom of the chassis hull
    // (the hull of the car's files, else the body mesh; the wheels are not part of it). The three in the middle of the centre column
    // are larger, and the three at the rear can be (the spoiler of a car sticks out there).
    probes.clear();
    probeRadii.clear();
    const Vec3f hullLo = hasHullBox ? hullBoxMin : bmin, hullHi = hasHullBox ? hullBoxMax : bmax;
    const float hullW = hullHi.x - hullLo.x, hullL = hullHi.z - hullLo.z;
    const float baseR = std::min(P.probeRadius, std::min(hullW / 6.5f, hullL / 11.0f)) * C.sphereScale;
    const float middleR = baseR * C.middleSphereScale, rearR = baseR * C.rearSphereScale;
    const bool rearAtLowZ = 0.5f * (wheelRest[2].z + wheelRest[3].z) < 0.5f * (wheelRest[0].z + wheelRest[1].z);  // the rear axle is behind
    for (int row = 0; row < 5; row++) {  // row 0 is the rear one
        const int iz = rearAtLowZ ? row : 4 - row;
        for (int ix = 0; ix < 3; ix++) {
            const float r = row == 0 ? rearR : (ix == 1 && row >= 1 && row <= 3) ? middleR : baseR;
            const float x = 0.5f * (hullLo.x + hullHi.x) + (ix - 1) * (0.5f * hullW - r);
            float z = hullLo.z + baseR + (hullL - 2 * baseR) * (float)iz / 4.0f;
            if (row == 0) z = rearAtLowZ ? hullLo.z + r : hullHi.z - r;  // the big ones stay inside the hull at the rear
            probes.push_back(Vec3f(x, hullLo.y + r, z));
            probeRadii.push_back(r);
        }
    }
    // pos is the centre of mass: when that moves within the car, move pos with it so that the car itself stays put
    updateAxes();
    pos += axes[0] * (centreOfMass.x - previousCentreOfMass.x) + axes[1] * (centreOfMass.y - previousCentreOfMass.y) + axes[2] * (centreOfMass.z - previousCentreOfMass.z);
}

void Vehicle::reset(const Vec3f& groundPos, float yaw) {
    float axleY = 0.25f * (wheelRest[0].y + wheelRest[1].y + wheelRest[2].y + wheelRest[3].y);
    // car origin height so that the wheels touch the road with the suspension at its rest length
    Vec3f origin = groundPos + Vec3f(0, wheelR - axleY + 0.02f * sizeScale_, 0);
    q = quatNormalize({std::cos(yaw * 0.5f), 0, std::sin(yaw * 0.5f), 0});
    updateAxes();
    // pos is the centre of mass; the car origin is pos - R * centreOfMass
    pos = origin + axes[0] * centreOfMass.x + axes[1] * centreOfMass.y + axes[2] * centreOfMass.z;
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
    Vec3f origin = pos - (axes[0] * centreOfMass.x + axes[1] * centreOfMass.y + axes[2] * centreOfMass.z);
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
    if (in.jump) vel += Vec3f(0, kangarooSpeed_, 0);  // an impulse, once per press: straight up whatever the car's attitude
    probeTouched_.assign(probes.size(), 0);
    probeImpulse_.assign(probes.size(), Vec3f());
    torqueHull_ = Vec3f();
    probeContact_.resize(probes.size());
    for (int i = 0; i < n; i++) substep(h, in, world);
    probeForce_.resize(probes.size());
    for (size_t i = 0; i < probes.size(); i++) probeForce_[i] = probeImpulse_[i] * (1.0f / dt);
}

// One physics step of `stepSeconds`. Forces and torques are collected over the whole car (they act on the centre of mass, `pos`)
// and then integrated: first the four wheels (suspension, engine, brakes, tyre grip), then the chassis against the level.
void Vehicle::substep(float stepSeconds, const VehicleInput& in, const CollisionWorld& world) {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    updateAxes();
    const Vec3f rightAxis = axes[0], upAxis = axes[1], forwardAxis = axes[2];  // the car's own directions, in world space
    const float speed = length(vel);                     // m/s, in any direction
    const float forwardSpeed = dot(vel, forwardAxis);    // m/s along the nose: negative when rolling backwards

    // --- steering: full lock at a standstill, a fraction of it at speed; the wheels turn towards that angle smoothly
    const float maxSteerAngle = P.maxSteerAngle / (1.0f + std::pow(speed / C.steerFadeSpeed, P.steerFadeExponent));  // radians
    const float targetSteerAngle = clampf(in.steer, -1.0f, 1.0f) * maxSteerAngle;
    steerAngle += (targetSteerAngle - steerAngle) * std::min(1.0f, stepSeconds * P.steerResponse);

    // --- engine and brake intent
    float driveForce = 0;                  // N the engine pushes the car with, summed over the driven wheels (negative: reverse)
    float brakeInput = in.brake;           // 0..1
    const float handbrakeInput = in.handbrake;  // 0..1
    if (in.accel > P.minPedal) {
        // the engine's push is limited by its force at low speed and by its power (force * speed) at high speed
        const float maxDriveForce = std::min(engineForce_, enginePower_ / std::max(std::fabs(forwardSpeed), C.powerMinSpeed));
        driveForce = in.accel * maxDriveForce;
        if (forwardSpeed < -1.0f) {  // accelerating while rolling backwards: brake first
            brakeInput = std::max(brakeInput, in.accel);
            driveForce = 0;
        }
    } else if (in.brake > P.minPedal && forwardSpeed < 1.0f) {  // the brake pedal doubles as reverse once the car has (almost) stopped
        driveForce = -in.brake * std::min(engineForce_ * P.reverseForceFraction,
                                          enginePower_ * P.reversePowerFraction / std::max(std::fabs(forwardSpeed), C.reversePowerMinSpeed));
        brakeInput = 0;
    }
    // the engine's push fades out over a speed range, so it has a soft top speed of its own (the afterburner is not affected)
    if (std::fabs(forwardSpeed) > C.taperStartSpeed && driveForce * forwardSpeed > 0)
        driveForce *= std::max(0.0f, 1.0f - (std::fabs(forwardSpeed) - C.taperStartSpeed) / C.taperLength);

    Vec3f totalForce(0, -gravity() * mass, 0);  // N, on the centre of mass: starts with gravity
    Vec3f totalTorque;                         // N m, about the centre of mass
    // air drag against the velocity, and downforce pressing the car down onto its wheels
    dragForce_ = vel * (-dragCoefficient_ * speed);
    // The downforce grows like the square of the speed at first and then in proportion to it: rate * (sqrt(v^2 + knee^2) - knee), smooth all
    // the way and without the runaway of a square at high speed.
    const float downforceMagnitude = C.downforceRate * (std::sqrt(speed * speed + C.downforceKnee * C.downforceKnee) - C.downforceKnee);
    downforce_ = upAxis * -downforceMagnitude;
    thrustForce_ = in.afterburner ? forwardAxis * afterburnerForce_ : Vec3f();  // a rocket along the car's nose
    gravityForce_ = Vec3f(0, -gravity() * mass, 0);
    totalForce += dragForce_ + downforce_ + thrustForce_;

    // --- the wheels
    groundedCount = 0;
    float largestSlipAngle = 0;  // radians, over the wheels on the ground: how sideways the car slides (see the drift measure below)
    for (int i = 0; i < 4; i++) {
        Wheel& wheel = wheels[i];
        const bool isFrontWheel = i < 2, isRearWheel = !isFrontWheel;  // wheels 0, 1 are the front ones, 2, 3 the rear (drive) ones
        // The suspension is a ray from its upper mount straight down (along the car's down axis): where it hits the road,
        // and how much of the suspension is left, tells how far the spring is compressed.
        const Vec3f wheelOffsetFromCentreOfMass = wheelRest[i] - centreOfMass;  // in car space
        const Vec3f suspensionMount = pos + rightAxis * wheelOffsetFromCentreOfMass.x +
                                      upAxis * (wheelOffsetFromCentreOfMass.y + suspRest_) +
                                      forwardAxis * wheelOffsetFromCentreOfMass.z;  // world position of the upper end of the suspension
        // The ray starts a bit above the mount (more for the cars with more travel): a road surface that is higher than the mount
        // (a curb, a wall the car drives against) is then still found from above, instead of the ray starting inside it or below it.
        const float rayLift = C.rayLift;
        const Vec3f rayOrigin = suspensionMount + upAxis * rayLift;
        const float suspensionRayLength = suspMax_ + wheelRadii[i] + rayLift;  // fully extended suspension plus the wheel's radius
        float roadFriction = 1.0f;  // the surface's tyre friction multiplier (Attribute files: grass 0.8, road 1.0, ...)
        float rayHitDistance;     // from the mount to the road
        Vec3f groundNormal;       // unit normal of the road where the ray hit
        wheel.grounded = false;
        wheel.load = 0;
        float suspensionLength = suspMax_;  // from the mount to the wheel centre; the maximum when nothing is under the wheel
        if (world.raycast(rayOrigin, upAxis * -1.0f, suspensionRayLength, false, rayHitDistance, groundNormal, &roadFriction)) {
            rayHitDistance -= rayLift;  // measured from the mount
            suspensionLength = std::max(suspMin_, rayHitDistance - wheelRadii[i]);
            if (suspensionLength < suspMax_) wheel.grounded = true;
        }
        if (wheel.grounded) {
            groundedCount++;
            const Vec3f contactPoint = suspensionMount - upAxis * rayHitDistance;
            const Vec3f contactOffset = contactPoint - pos;  // from the centre of mass to where the tyre touches the road
            // Bottoming out: the wheel is past the end of the suspension's travel. That end is a solid stop, not a spring: the part of the
            // speed that still presses the tyre into the road is taken away at once (an inelastic collision, nothing comes back), and
            // the car is lifted out of the overlap without any speed, so it does not bounce.
            const float bottomedBy = wheelRadii[i] + suspMin_ - rayHitDistance;  // m past the stop, > 0 when bottomed out
            if (bottomedBy > 0) {
                const Vec3f pressLever = contactOffset;  // where the road holds the tyre, from the centre of mass
                const float pressSpeed = dot(vel + cross(angVel, pressLever), upAxis);  // < 0: moving down into the road
                if (pressSpeed < 0) {
                    const Vec3f turning = cross(pressLever, upAxis);
                    const float stopImpulse = -pressSpeed / (1.0f / mass + dot(turning, invInertia(turning)));
                    vel += upAxis * (stopImpulse / mass);
                    angVel += invInertia(cross(pressLever, upAxis * stopImpulse));
                    impact_ = std::max(impact_, -pressSpeed);
                }
                pos += upAxis * (bottomedBy * P.bottomOutPushOut);
            }
            const Vec3f mountVelocity = vel + cross(angVel, suspensionMount - pos);  // speed of the suspension's upper end

            // spring + damper (stiffer at the bump stop); a suspension cannot pull the car down, so never negative
            // The spring is free a little longer when the downforce presses on it, so the ride height stays (a fraction of the compression
            // the downforce would make in a quarter of the car's spring); the bump stop stiffens smoothly over its zone.
            const float springFreeNow = springFree_ + C.downforceHeightCompensation * downforceMagnitude * 0.25f / std::max(1.0f, springStiffness);
            const float springCompression = springFreeNow - suspensionLength;
            const float bumpDepth = clampf((suspMin_ + bumpZone_ - suspensionLength) / std::max(1e-4f, bumpZone_), 0.0f, 1.0f);  // 0: above the zone, 1: at the stop
            const float bumpStiffness = 1.0f + (P.bumpStopStiffness - 1.0f) * bumpDepth * bumpDepth;
            float suspensionForce = springStiffness * springCompression * bumpStiffness +
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
            // The mass this tyre moves when it pushes along a direction at its contact point: the whole car, but a push off the centre
            // of mass also turns the car, which lets the point move more easily. That is what the stopping forces below are made for
            // (the force that would cancel the point's speed within one step); a quarter of the mass, as used before, overshoots when the
            // four wheels turn the car together. And only a part of it is asked for (tyreStopFraction), as the other wheels push too.
            const Vec3f tyreLever = contactOffset + upAxis * P.rollCentre;  // where the tyre force acts, from the centre of mass
            auto effectiveMass = [&](const Vec3f& direction) {
                const Vec3f turning = cross(tyreLever, direction);
                const float inverseMass = 1.0f / mass + dot(turning, invInertia(turning));
                return 1.0f / std::max(inverseMass, 1e-6f);
            };
            const float forwardStopMass = effectiveMass(wheelForwardOnRoad) * P.tyreStopFraction;
            const float sidewaysStopMass = effectiveMass(wheelSidewaysOnRoad) * P.tyreStopFraction;

            // Force along the rolling direction: engine on the rear wheels (half each), brakes, rolling resistance.
            float longitudinalForce = 0;
            if (isRearWheel) longitudinalForce += driveForce * 0.5f;
            // The original game takes the friction from the static weight of a quarter of the car, not from the wheel's current
            // load, so load transfer does not change the grip (no feedback that lifts the nose or tips the car over).
            const float staticWheelLoad = mass * gravity() * 0.25f;
            const float frictionLoad = staticWheelLoad + (suspensionForce - staticWheelLoad) * P.tyreLoadSensitivity;
            const float frictionLimit = grip_ * (1.0f + (roadFriction - 1.0f) * P.surfaceFrictionEffect) * frictionLoad;  // N: the most the tyre can push with, in any direction
            float brakingForce = brakeInput * P.brakeForce * frictionLimit;
            if (isRearWheel) brakingForce = std::max(brakingForce, handbrakeInput * P.handbrakeBrake * frictionLimit);
            const float rollingDirection = longitudinalSpeed > 0 ? 1.0f : -1.0f;  // brakes and resistance act against the motion
            const float maxBrakingForce = std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds;  // never brake past a standstill
            longitudinalForce -= rollingDirection * std::min(brakingForce, maxBrakingForce) * std::min(1.0f, std::fabs(longitudinalSpeed) * 2.0f);
            const float rollingResistanceForce = std::min(P.rollResist * suspensionForce, std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds);
            longitudinalForce -= rollingDirection * rollingResistanceForce * (std::fabs(driveForce) > 1.0f ? P.rollResistWhileDriving : 1.0f);

            // Force across the wheel: the force that would stop the sideways slide within one step, limited by the grip
            // (the rear wheels lose grip with the handbrake); a tyre over the limit slides and keeps only part of it.
            const float handbrakeGripFactor = (isRearWheel && handbrakeInput > 0.05f) ? 1.0f - P.handbrakeGripLoss * handbrakeInput : 1.0f;
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
            longitudinalForce = clampf(longitudinalForce, -longitudinalFrictionLimit, longitudinalFrictionLimit);

            const Vec3f tyreForce = wheelForwardOnRoad * longitudinalForce + wheelSidewaysOnRoad * lateralForce;
            totalForce += tyreForce;
            wheel.contactPoint = contactPoint;
            wheel.force = suspensionForceVector + tyreForce;
            const Vec3f tyreForceOffset = tyreLever;  // acts a little above the road: less body roll
            totalTorque += cross(tyreForceOffset, tyreForce);

            wheel.lateralForce = lateralForce;
            wheel.longitudinalForce = longitudinalForce;
            wheel.frictionLimit = frictionLimit;
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
            wheel.longitudinalForce = wheel.frictionLimit = 0;
            wheel.force = Vec3f();
            wheel.sliding = false;
            if (isFrontWheel) wheel.steer = steerAngle;
        }
        wheel.centre = wheelRest[i] + Vec3f(0, suspRest_ - suspensionLength, 0);  // for drawing: lower when the suspension is extended
    }
    torqueWheels_ = totalTorque;  // (only the wheels' forces carry a torque so far)
    // drift: 0 up to a slip angle that is just cornering, then rising to 1; never while slow
    drift_ = speed > C.driftMinSpeed ? clampf((largestSlipAngle - P.driftSlipDeadZone) / P.driftSlipRange, 0.0f, 1.0f) : 0.0f;

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
    const Vec3f carOrigin = pos - (axes[0] * centreOfMass.x + axes[1] * centreOfMass.y + axes[2] * centreOfMass.z);
    for (size_t probeIndex = 0; probeIndex < probes.size(); probeIndex++) {
        const Vec3f& probeInCarSpace = probes[probeIndex];
        const Vec3f probeCentre = carOrigin + axes[0] * probeInCarSpace.x + axes[1] * probeInCarSpace.y + axes[2] * probeInCarSpace.z;
        Vec3f contactNormal;   // from the level towards the probe
        float penetrationDepth;
        const float sphereRadius = probeRadius((int)probeIndex);
        if (!world.sphereContact(probeCentre, sphereRadius, contactNormal, penetrationDepth)) continue;
        if (probeIndex < probeTouched_.size()) {
            probeTouched_[probeIndex] = 1;
            probeContact_[probeIndex] = probeCentre - contactNormal * sphereRadius;
        }
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
            if (probeIndex < probeImpulse_.size()) probeImpulse_[probeIndex] += contactNormal * normalImpulse;
            torqueHull_ += cross(probeOffset, contactNormal * normalImpulse) * (1.0f / stepSeconds);
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
                if (probeIndex < probeImpulse_.size()) probeImpulse_[probeIndex] += tangentDirection * -frictionImpulse;
                vel += tangentDirection * (-frictionImpulse / mass);
                angVel += invInertia(cross(probeOffset, tangentDirection * -frictionImpulse));
                torqueHull_ += cross(probeOffset, tangentDirection * -frictionImpulse) * (1.0f / stepSeconds);
            }
        }
        pos += contactNormal * std::min(penetrationDepth * P.hullPushOut, C.hullMaxPushOut);  // out of the surface
    }
}

}  // namespace minigame01
