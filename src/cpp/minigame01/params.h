// The numbers the driving minigame's physics and rules run on. Their values are not defined here: the configuration manager
// (src/ts/config) owns the defaults, reads what the player edits and computes the final numbers, and sends them as two objects of
// "name": number, `globals` (the same for every car) and `car` (computed for the car that is being driven, from its game files and
// the configuration). Every name below must be there; a missing or unknown one is an error. Units: metres, seconds, kilograms, newtons,
// radians, unless said otherwise.
#pragma once
#include <string>

namespace minigame01 {

// Values that are the same for every car.
#define MINIGAME_GLOBALS(X) \
    X(rollInertiaFactor) \
    X(probeRadius) \
    X(gravity) \
    X(maxPhysicsStep) \
    X(vehicleMaxFrameSeconds) \
    X(bottomOutPushOut) \
    X(bumpStopStiffness) \
    X(tyreLoadSensitivity) \
    X(surfaceFrictionEffect) \
    X(tyreSlipCurve) \
    X(tyreSlipKnee) \
    X(tyreSlipKneeValue) \
    X(tyreSlipFull) \
    X(tyreStopFraction) \
    X(slideGrip) \
    X(brakeForce) \
    X(rollResist) \
    X(maxSpeed) \
    X(rollCentre) \
    X(maxSteerAngle) \
    X(steerFadeExponent) \
    X(steerResponse) \
    X(minPedal) \
    X(reverseForceFraction) \
    X(reversePowerFraction) \
    X(handbrakeBrake) \
    X(handbrakeGripLoss) \
    X(rollResistWhileDriving) \
    X(lateralStiffness) \
    X(airAngularDamping) \
    X(groundAngularDamping) \
    X(driftSlipDeadZone) \
    X(driftSlipRange) \
    X(driftPointsFactor) \
    X(floorNormalY) \
    X(wallBounce) \
    X(floorFriction) \
    X(wallFriction) \
    X(hullPushOut) \
    X(roundSeconds) \
    X(pointsPowerup) \
    X(pointsNotable) \
    X(pointsTimeBonus) \
    X(timeBonusSeconds) \
    X(pointsUpgrade) \
    X(pointsCredit) \
    X(pointsPickup) \
    X(pointsStar) \
    X(finishBonusPerSecond) \
    X(maxFrameSeconds) \
    X(safeSpotSeconds) \
    X(stuckSeconds) \
    X(worldFloorBelow) \
    X(cameraSpeedDistance) \
    X(cameraYawFollow) \
    X(cameraPositionFollow) \
    X(cameraSlideBlend) \
    X(keySteerRise) \
    X(keySteerReturn) \
    X(keySteerReverse) \
    X(keyAccelRise) \
    X(keyAccelFall) \
    X(keyBrakeRise) \
    X(keyBrakeFall) \
    X(gamepadDeadzone)

// The values of the car that is driven, final (see src/ts/config/config.ts, resolveCar).
#define MINIGAME_CAR(X) \
    X(toy) \
    X(sizeScale) \
    X(mass) \
    X(centreOfMassX) \
    X(centreOfMassY) \
    X(centreOfMassZ) \
    X(engineForce) \
    X(enginePower) \
    X(grip) \
    X(drag) \
    X(afterburnerForce) \
    X(kangarooSpeed) \
    X(springK) \
    X(damper) \
    X(suspMin) \
    X(suspMax) \
    X(suspRest) \
    X(springFree) \
    X(bumpStopZone) \
    X(rayLift) \
    X(wheelRadius0) \
    X(wheelRadius1) \
    X(wheelRadius2) \
    X(wheelRadius3) \
    X(steerFadeSpeed) \
    X(taperStartSpeed) \
    X(taperLength) \
    X(slipFloorSpeed) \
    X(downforceRate) \
    X(downforceKnee) \
    X(downforceHeightCompensation) \
    X(sphereScale) \
    X(middleSphereScale) \
    X(rearSphereScale) \
    X(powerMinSpeed) \
    X(reversePowerMinSpeed) \
    X(driftMinSpeed) \
    X(hullMaxPushOut) \
    X(stuckMaxSpeed) \
    X(pickupRadius) \
    X(pickupReachY) \
    X(forceArrowLength) \
    X(dropScale) \
    X(cameraDistance) \
    X(cameraHeight) \
    X(cameraFarDistance) \
    X(cameraFarHeight) \
    X(cameraLookAhead) \
    X(cameraLookHeight) \
    X(cameraWallMargin) \
    X(cameraMinDistance) \
    X(cameraWallCheckLength) \
    X(cameraSlideMinSpeed) \
    X(cameraSpeedCap)

struct Params {
#define X(name) float name = 0;
    MINIGAME_GLOBALS(X)
#undef X
};

struct CarSettings {
#define X(name) float name = 0;
    MINIGAME_CAR(X)
#undef X
};

// The values in effect (one set for the whole minigame).
Params& tuning();
CarSettings& carSettings();

// Reads {"globals": {...}, "car": {...}} (objects of "name": number) and makes both effective. False with a message (the line number
// included) when the text is not valid, a name is unknown or one is missing; nothing changes then.
bool settingsFromJson(const std::string& text, std::string& error);

}  // namespace minigame01
