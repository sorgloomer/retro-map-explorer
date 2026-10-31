#include "minigame01/vehicle.h"
#include "math/functions.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

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

// How much stiffer than its normal spring the suspension is at this length: the bump stop stiffens smoothly over its zone (1 above it).
float Vehicle::bumpStiffnessAt(float suspensionLength) const {
    const float depth = clamp((bumpZone_ - suspensionLength) / std::max(1e-4f, bumpZone_), 0.0f, 1.0f);  // 0: above the zone, 1: at the stop
    return 1.0f + (tuning().bumpStopStiffness - 1.0f) * depth * depth;
}

// Takes the values of the car from the settings (computed by the configuration manager, see params.h) and works out the inertia and
// the hull probes. Called when the car is set up and whenever the settings change; the car stays where it is (its origin does not move).
void Vehicle::applyTuning() {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    const Vec3f previousCentreOfMass = centreOfMass;
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
    springStiffness = C.suspensionSpringK;
    damping = C.suspensionDamperCoefficient;
    // The length of the spring in isolation: the one at which the car's weight, shared by the wheels, compresses it to the rest length of
    // the suspension (so the car stands at its rest pose, whatever its mass, gravity and spring).
    suspensionSpringNaturalLength_ = suspensionLengthAtRest_ + mass * gravity_ / (kWheelCount * std::max(1.0f, springStiffness) * bumpStiffnessAt(suspensionLengthAtRest_));
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
    const float baseR = std::min(P.probeRadius, std::min(hullW / 6.5f, hullL / 11.0f)) * C.hullSphereScale;
    const float middleR = baseR * C.hullMiddleSphereScale, rearR = baseR * C.hullRearSphereScale;
    const bool rearAtLowZ = 0.5f * (wheelRest[2].z + wheelRest[3].z) < 0.5f * (wheelRest[0].z + wheelRest[1].z);  // the rear axle is behind
    for (int row = 0; row < 5; row++) {  // row 0 is the rear one
        const int iz = rearAtLowZ ? row : 4 - row;
        for (int ix = 0; ix < 3; ix++) {
            const float r = row == 0 ? rearR : (ix == 1 && row >= 1 && row <= 3) ? middleR : baseR;
            const float x = 0.5f * (hullLo.x + hullHi.x) + static_cast<float>(ix - 1) * (0.5f * hullW - r);
            float z = hullLo.z + baseR + (hullL - 2 * baseR) * static_cast<float>(iz) / 4.0f;
            if (row == 0) z = rearAtLowZ ? hullLo.z + r : hullHi.z - r;  // the big ones stay inside the hull at the rear
            probes.emplace_back(x, hullLo.y + r, z);
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
    q = normalize({std::cos(yaw * 0.5f), 0, std::sin(yaw * 0.5f), 0});
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

void Vehicle::liftAndRoll(float metres) {
    constexpr float kHalfAngleSin = 0.70710678f, kHalfAngleCos = 0.70710678f;  // half of 90 degrees
    q = normalize(q * Quat{kHalfAngleCos, 0, 0, kHalfAngleSin});  // turn about the car's own forward (z) axis
    pos.y += metres;
    updateAxes();
}

void Vehicle::updateAxes() {
    axes[0] = rotate(q, Vec3f(1, 0, 0));
    axes[1] = rotate(q, Vec3f(0, 1, 0));
    axes[2] = rotate(q, Vec3f(0, 0, 1));
}

Mat4f Vehicle::modelMatrix() const {
    Mat4f m = Mat4f::identity();
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
    const float steerFadeSpeedFraction = std::abs(speed) / C.steerFadeSpeed;  // (never 0: 1 / 0 is infinite, and 0 * infinity is NaN, which would stay in steerAngle)
    const float steerAngleMaxAtSpeed = C.steerAngleMax * mix(1.0f, 1.0f / (1.0f + steerFadeSpeedFraction), smoothstep(steerFadeSpeedFraction));  // radians
    const float targetSteerAngle = clamp(in.steer, -1.0f, 1.0f) * steerAngleMaxAtSpeed;
    steerAngle += (targetSteerAngle - steerAngle) * std::min(1.0f, stepSeconds * C.steerResponse);

    // --- engine and brake intent
    constexpr float kMinPowerSpeed = 1e-3f;  // m/s: only so that power / speed is finite when the car stands still
    float driveForce = 0;                  // N the engine pushes the car with, summed over the driven wheels (negative: reverse)
    float brakeInput = in.brake;           // 0..1
    const float handbrakeInput = in.handbrake;  // 0..1
    if (in.accel > P.minPedal) {
        // the engine's push is limited by its force at low speed and by its power (force * speed) at high speed
        const float maxDriveForce = std::min(engineForce_, enginePower_ / std::max(std::fabs(forwardSpeed), kMinPowerSpeed));
        driveForce = in.accel * maxDriveForce;
        if (forwardSpeed < -1.0f) {  // accelerating while rolling backwards: brake first
            brakeInput = std::max(brakeInput, in.accel);
            driveForce = 0;
        }
    } else if (in.brake > P.minPedal && forwardSpeed < 1.0f) {  // the brake pedal doubles as reverse once the car has (almost) stopped
        driveForce = -in.brake * std::min(engineForce_ * P.reverseForceFraction,
                                          enginePower_ * P.reversePowerFraction / std::max(std::fabs(forwardSpeed), kMinPowerSpeed));
        brakeInput = 0;
    }
    // the engine's push fades out over a speed range, so it has a soft top speed of its own (the afterburner is not affected)
    if (std::fabs(forwardSpeed) > C.engineTaperStartSpeed && driveForce * forwardSpeed > 0)
        driveForce *= std::max(0.0f, 1.0f - (std::fabs(forwardSpeed) - C.engineTaperStartSpeed) / C.engineTaperLength);

    Vec3f totalForce(0, -gravity() * mass, 0);  // N, on the centre of mass: starts with gravity
    Vec3f totalTorque;                         // N m, about the centre of mass
    // air drag against the velocity, and downforce pressing the car down onto its wheels
    dragForce_ = vel * (-dragCoefficient_ * speed);
    // The downforce grows like the square of the speed at first and then in proportion to it: rate * (sqrt(v^2 + knee^2) - knee), smooth all
    // the way and without the runaway of a square at high speed.
    const float downforceMagnitude = C.downforceRate * (std::sqrt(speed * speed + C.downforceKnee * C.downforceKnee) - C.downforceKnee);
    downforce_ = upAxis * -downforceMagnitude;
    thrustForce_ = Vec3f();  // rockets at the centre of mass: the afterburner along the car's nose, the side thrust towards its right
    if (in.afterburner) thrustForce_ += forwardAxis * afterburnerForce_;
    if (in.sideThrust) thrustForce_ += rightAxis * afterburnerForce_;
    gravityForce_ = Vec3f(0, -gravity() * mass, 0);
    totalForce += dragForce_ + downforce_ + thrustForce_;

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
    } contact[4];
    const float staticWheelLoad = mass * gravity() / kWheelCount;  // N: the car's weight shared by the wheels
    // The upper end of a suspension in the world, from the car's position now.
    auto suspensionMountOf = [&](int i) {
        const Vec3f wheelOffsetFromCentreOfMass = wheelRest[i] - centreOfMass;  // in car space
        return pos + rightAxis * wheelOffsetFromCentreOfMass.x + upAxis * (wheelOffsetFromCentreOfMass.y + suspensionLengthInLoadedModel_) +
               forwardAxis * wheelOffsetFromCentreOfMass.z;
    };

    // --- pass 1: the road under the wheels, and the bottom-out
    for (int i = 0; i < 4; i++) {
        Wheel& wheel = wheels[i];
        WheelContact& c = contact[i];
        c.suspensionLength = suspensionLengthMax_;
        wheel.grounded = false;
        wheel.load = 0;
        // The suspension is a ray from its upper mount straight down (along the car's down axis): where it hits the road,
        // and how much of the suspension is left, tells how far the spring is compressed.
        const Vec3f suspensionMount = suspensionMountOf(i);
        // The ray starts a bit above the mount (more for the cars with more travel): a road surface that is higher than the mount
        // (a curb, a wall the car drives against) is then still found from above, instead of the ray starting inside it or below it.
        const float rayLift = C.rayLift;
        const Vec3f rayOrigin = suspensionMount + upAxis * rayLift;
        const float suspensionRayLength = suspensionLengthMax_ + wheelRadii[i] + rayLift;  // fully extended suspension plus the wheel's radius
        float rayHitDistance;     // from the mount to the road
        if (!world.raycast(rayOrigin, upAxis * -1.0f, suspensionRayLength, false, rayHitDistance, c.groundNormal, &c.roadFriction)) continue;
        rayHitDistance -= rayLift;  // measured from the mount
        c.suspensionLength = std::max(0.0f, rayHitDistance - wheelRadii[i]);
        if (c.suspensionLength >= suspensionLengthMax_) continue;
        wheel.grounded = c.grounded = true;
        groundedCount++;
        c.contactPoint = suspensionMount - upAxis * rayHitDistance;
        // Bottoming out: the wheel is past the end of the suspension's travel. That end is a solid stop, not a spring: the part of the
        // speed that still presses the tyre into the road is taken away at once (an inelastic collision, nothing comes back), and
        // the car is lifted out of the overlap without any speed, so it does not bounce.
        const float bottomedBy = wheelRadii[i] - rayHitDistance;  // m past the stop, > 0 when bottomed out
        if (bottomedBy > 0) {
            const Vec3f pressLever = c.contactPoint - pos;  // where the road holds the tyre, from the centre of mass
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
    }

    // --- pass 2: the suspension forces
    float suspensionForceSum = 0;  // N, over the wheels on the ground
    for (int i = 0; i < 4; i++) {
        if (!contact[i].grounded) continue;
        WheelContact& c = contact[i];
        const Vec3f suspensionMount = suspensionMountOf(i);
        const Vec3f contactOffset = c.contactPoint - pos;  // from the centre of mass to where the tyre touches the road
        const Vec3f mountVelocity = vel + cross(angVel, suspensionMount - pos);  // speed of the suspension's upper end
        // spring + damper (stiffer at the bump stop); a suspension cannot pull the car down, so never negative
        // The spring is free a little longer when the downforce presses on it, so the ride height stays (a fraction of the compression
        // the downforce would make in a quarter of the car's spring); the bump stop stiffens smoothly over its zone.
        const float suspensionSpringNaturalLengthNow = suspensionSpringNaturalLength_ + C.downforceHeightCompensation * downforceMagnitude / (kWheelCount * std::max(1.0f, springStiffness));
        const float springCompression = suspensionSpringNaturalLengthNow - c.suspensionLength;
        const float bumpStiffness = bumpStiffnessAt(c.suspensionLength);
        c.suspensionForce = std::max(0.0f, springStiffness * springCompression * bumpStiffness +
                                           damping * (-dot(mountVelocity, upAxis)));
        wheels[i].load = c.suspensionForce;  // N: how hard the tyre is pressed on the road
        suspensionForceSum += c.suspensionForce;
        // The road pushes the tyre perpendicular to its surface, at the contact point (not along the suspension: a pitched car on a
        // flat road would then be pushed forward by the horizontal part of the suspension force). The spring, the damper and the
        // bottom-out above still work along the car's up axis, the direction the suspension moves in.
        const Vec3f roadNormal = dot(c.groundNormal, upAxis) >= 0 ? c.groundNormal : c.groundNormal * -1.0f;  // towards the car
        const Vec3f suspensionForceVector = roadNormal * c.suspensionForce;
        totalForce += suspensionForceVector;
        totalTorque += cross(contactOffset, suspensionForceVector);
        wheels[i].force = suspensionForceVector;
    }

    // --- pass 3: the tyres
    for (int i = 0; i < 4; i++) {
        Wheel& wheel = wheels[i];
        const WheelContact& c = contact[i];
        const bool isFrontWheel = i < 2, isRearWheel = !isFrontWheel;  // wheels 0, 1 are the front ones, 2, 3 the rear (drive) ones
        if (c.grounded) {
            const Vec3f contactOffset = c.contactPoint - pos;  // from the centre of mass to where the tyre touches the road
            const float suspensionForce = c.suspensionForce;
            const float roadFriction = c.roadFriction;
            const Vec3f groundNormal = c.groundNormal;
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
            // gripMode: 0 physical (the wheel's own suspension force), 1 constant (the same share for every wheel on the ground), 2 weighted
            // (the whole weight of the car shared by the wheels on the ground in proportion to their suspension forces)
            float frictionLoad;
            if (C.gripMode > 1.5f) frictionLoad = suspensionForceSum > 1e-3f ? mass * gravity() * suspensionForce / suspensionForceSum : staticWheelLoad;
            else if (C.gripMode > 0.5f) frictionLoad = staticWheelLoad;
            else frictionLoad = staticWheelLoad + (suspensionForce - staticWheelLoad) * P.tyreLoadSensitivity;
            const float frictionLimit = grip_ * (1.0f + (roadFriction - 1.0f) * P.surfaceFrictionEffect) * frictionLoad;  // N: the most the tyre can push with, in any direction
            float brakingForce = brakeInput * P.brakeForce * frictionLimit;
            const bool handbrakeActsHere = isRearWheel || C.handbrakeAllWheels > 0.5f;
            if (handbrakeActsHere) brakingForce = std::max(brakingForce, handbrakeInput * P.handbrakeBrake * frictionLimit);
            const float rollingDirection = longitudinalSpeed > 0 ? 1.0f : -1.0f;  // brakes and resistance act against the motion
            const float maxBrakingForce = std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds;  // never brake past a standstill
            longitudinalForce -= rollingDirection * std::min(brakingForce, maxBrakingForce) * std::min(1.0f, std::fabs(longitudinalSpeed) * 2.0f);
            const float rollingResistanceForce = std::min(P.rollResist * suspensionForce, std::fabs(longitudinalSpeed) * forwardStopMass / stepSeconds);
            longitudinalForce -= rollingDirection * rollingResistanceForce * (std::fabs(driveForce) > 1.0f ? P.rollResistWhileDriving : 1.0f);

            // Force across the wheel: the force that would stop the sideways slide within one step, limited by the grip
            // (the rear wheels lose grip with the handbrake); a tyre over the limit slides and keeps only part of it.
            const float handbrakeGripFactor = (handbrakeActsHere && handbrakeInput > 0.05f) ? 1.0f - P.handbrakeGripLoss * handbrakeInput : 1.0f;
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
            totalForce += tyreForce;
            wheel.contactPoint = c.contactPoint;
            wheel.force += tyreForce;  // (the suspension force was put there in pass 2)
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
        wheel.centre = wheelRest[i] + Vec3f(0, suspensionLengthInLoadedModel_ - c.suspensionLength, 0);  // for drawing: lower when the suspension is extended
    }
    torqueWheels_ = totalTorque;  // (only the wheels' forces carry a torque so far)
    // drift: 0 up to a slip angle that is just cornering, then rising to 1; never while slow
    drift_ = speed > C.driftMinSpeed ? clamp((largestSlipAngle - P.driftSlipDeadZone) / P.driftSlipRange, 0.0f, 1.0f) : 0.0f;

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
    const Quat orientationRate = Quat{0, angVel.x, angVel.y, angVel.z} * q;
    q = normalize(q + orientationRate * (0.5f * stepSeconds));
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
            const float restitution = isFloor ? 0.0f : C.hullWallBounce;
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
        // out of the surface, but not all the way: the sphere keeps overlapping it by a hair, so the contact is found again next step
        constexpr float kKeptOverlap = 2e-3f;  // of the sphere's radius
        const float pushDepth = std::max(0.0f, penetrationDepth - kKeptOverlap * sphereRadius);
        pos += contactNormal * std::min(pushDepth * P.hullPushOut, C.hullMaxPushOut);
    }

}


}  // namespace minigame01
