// The defaults of every configuration value (see config.ts). Units: metres, seconds, kilograms, newtons, radians, unless said otherwise.
// Generated once from the old C++ parameter list; this file is the source of truth now.
import * as z from "zod/mini";
import {hasOwn} from "../utils/objects.ts";
import type {
  CarId,
  ConfigGlobalType,
  ConfigPartCarType,
  ConfigPartial
} from "./config.schema.ts";

/** Values that are not about a particular car. */
export const GLOBAL_DEFAULTS: ConfigGlobalType = {
  // --- shape of the cars
  rollInertiaFactor: 1.5,  // turns the roll (about the forward axis) inertia up or down
  // --- world and time step
  maxPhysicsStep: 0.006666667,  // s: a frame is split into physics steps of at most this (1/150)
  vehicleMaxFrameSeconds: 0.1,  // s: the longest frame the vehicle simulates in one call
  // --- suspension: lengths from the upper mount down the car's up axis
  bottomOutPushOut: 0.5,  // a wheel past the end of its travel: this part of the overlap is undone each step, without any speed (no bounce)
  bumpStopStiffness: 2,  // ...by this factor
  // --- suspension, tyres, engine and air
  tyreSlipCurve: 1,  // 1: sideways force follows the original game's slip curve; 0: the older stiffness model
  tyreSlipKnee: 0.05,  // slip (fraction of the speed) at which the curve is at tyreSlipKneeValue of the peak grip
  tyreSlipKneeValue: 0.495,  // part of the peak grip given at the knee
  tyreSlipFull: 0.15,  // slip at which the tyre gives its full grip
  suspensionRayLift: 0.3,  // wheel radii: the wheel rays start this far above the suspension's upper mount, so a curb or a slope that is higher than the mount is still found
  tyreStopFraction: 0.5,  // a tyre asks for at most this part of the force that would cancel its contact speed within one step (the other wheels push too: 1 would overshoot and shake the car)
  slideGrip: 0.85,  // part of the grip left while a tyre slides sideways
  brakeForce: 1,  // brake pedal fully down asks for this part of the friction limit
  rollResist: 0.016,  // rolling resistance, as a fraction of the tyre load
  maxSpeed: 500,  // m/s (hard limit)
  // --- steering
  // --- driving details
  controlMinPedal: 0.01,  // a pedal below this counts as released
  lateralStiffness: 0.85,  // how much of the sideways speed a tyre tries to cancel in one step (below the limit)
  // --- rotation damping (1/s): the spin loses this part of its speed every second
  angularDampingAir: 0.1,  // no wheel touches the road
  angularDampingGround: 0.1,  // at least one wheel does
  // --- hull contacts (the chassis against the level)
  floorNormalY: 0.7,  // a contact whose normal points up at least this much counts as floor, else as a wall
  floorFriction: 0.6,  // sliding friction on a floor contact, as a multiple of the normal impulse
  wallFriction: 0.5,  // and on a wall contact
  hullPushOut: 0.7,  // how much of the penetration depth is corrected in one step...
  hullMaxPushOut: 0.25,  // ...but never more than this many metres
  // --- rules
  roundSeconds: 90,  // s on the clock in Powerup Rush
  pickupRadius: 3.4,  // m: a powerup is collected this close to the car (horizontally)...
  pickupReachY: 3.4,  // ...and this close vertically
  pointsPowerup: 50,  // score of an ordinary powerup (also pedestrian ones)
  pointsNotable: 150,  // notable powerups and mission items
  pointsTimeBonus: 50,  // time bonus powerups, which also add timeBonusSeconds
  timeBonusSeconds: 15,
  pointsUpgrade: 500,
  pointsCredit: 300,
  pointsPickup: 100,
  pointsStar: 250,
  finishBonusPerSecond: 10,  // score per second left when every powerup is collected
  maxFrameSeconds: 0.05,  // s: the longest frame the game simulates in one call
  safeSpotSeconds: 0.7,  // s between remembering a safe spot to return to after falling out of the world
  worldFloorBelow: 60,  // m below the level's lowest point: a car below it fell out of the world
  // --- camera
  cameraDistance: 6.4,  // m behind the car (chase camera)...
  cameraHeight: 2.5,  // ...and above it
  cameraFarDistance: 11,  // the same for the far camera (C)
  cameraFarHeight: 5,
  forceArrowLength: 4,  // debug arrows of the wheel forces: metres per car weight (a wheel carries a quarter of it at rest)
  cameraSpeedDistance: 0.035,  // m of extra distance per m/s of speed...
  cameraSpeedCap: 40,  // ...up to this speed
  cameraYawFollow: 3.2,  // 1/s: how quickly the camera turns behind the car
  cameraPositionFollow: 3,  // 1/s: how quickly it follows the wanted position (the natural frequency of the camera's spring)
  cameraPositionDampingRatio: 1,  // damping of that spring: 1 settles fastest without swinging past; below 1 the camera overshoots a little (more inertia)
  cameraLookAhead: 2.2,  // m ahead of the car that it looks at
  cameraLookHeight: 0.9,  // m above the car that it looks at
  cameraSlideMinSpeed: 5,  // m/s: faster than this, the camera looks where the car goes when it slides...
  cameraSlideBlend: 0.6,  // ...by this part of the difference between heading and movement
  cameraWallMargin: 0.4,  // m kept between the camera and a wall behind the car
  // --- keyboard feel (1/s: how fast the digital keys move the analog value)
  keySteerRise: 6.4,  // steering towards a key that is held
  keySteerReturn: 14,  // steering back to the centre when no key is held
  keySteerReverse: 18,  // steering from one side to the other
  keyAccelRise: 5,
  keyAccelFall: 8,
  keyBrakeRise: 7,
  keyBrakeFall: 10,
  gamepadDeadzone: 0.12,  // the stick's dead zone, 0..1
};

/**
 * Values that every car starts from, unless the car sets its own: a car value that is missing or null is taken from here, so these
 * can be changed for all cars at once ("carDefaults" in the JSON).
 */

export const CAR_DEFAULTS_COMMON = {
  cameraScale: 1,  // the chase camera's distance and height are multiplied by this (on top of the size of the car)

  downforceHeightCompensation: 1,  // 1: the springs are free just that much longer under the downforce, so the ride height stays; 0: the car sinks
  tractionControl: 1,  // 1: a driven wheel that is not sliding sideways or running against the drive does not spin faster than the road goes by (no burnouts); 0: it can
  handbrakeAllWheels: 1,  // 1: the handbrake brakes (and loosens the grip of) all four wheels; 0: only the rear ones
  suspensionDamperCoefficientSource: "file",
  suspensionDamperCoefficient: 3300, // N s/m
  suspensionDamperCoefficientAsFractionOfCritical: 0.85,  // damper as a part of the critical damping of the spring (1 = no overshoot at all); 0: use suspensionDamperCoefficient
  suspensionSpringK: 30000,  // N/m: used when the spring constant is not taken from the car's files
  suspensionSpringKFromFileEnabled: 1,  // 1: the spring constant is the one of the car's game files (when it has one) times suspensionSpringKFromFileRatio; 0: suspensionSpringK
  suspensionSpringKFromFileRatio: 1,  // the file's spring constant is multiplied by this
  hullWallBounce: 0.1,  // restitution of the hull against walls and ceilings (floors do not bounce)
  driftMinSpeed: 8,  // m/s (for a car of 4.3 m: smaller cars scale it down): slower than this the car is never drifting
  driftSlipDeadZone: 0.12,  // radians of slip angle that is just cornering
  driftSlipRange: 0.45,  // radians more of slip angle for the measure to go from 0 to 1
  driftPointsFactor: 0.5,  // score per (drift measure * m/s * second)
  rendererCarBrightness: 0.5,  // the sun shines this many times as bright on the car (multiplies the colour of the sun for the lit car; 1: as the level's lighting makes it)
  reverseForceFraction: 0.55,  // reverse has this part of the engine force...
  reversePowerFraction: 0.4,  // ...and of the power
  handbrakeBrake: 0.85,  // the handbrake brakes the wheels it acts on with this part of the friction limit
  handbrakeGripLoss: 0.62,  // and takes this part of their sideways grip away (the car slides)
  hullProbeRadius: 0.3,  // m, radius of the spheres that make the chassis collide with walls and ceilings (a small car has smaller ones: they never exceed what fits its hull)
  steerAngleCentrifugalKneeFactor: 0.3,  // how softly the steering angle at speed bends into its limit: the width of the knee as a fraction of steerAngleMax (0: a sharp corner)
  steerAngleCentrifugalLimitFactor: 0.7,  // multiplies the sideways force the tyres can hold before slipping, which limits the steering angle at speed (0: no turning at speed)
  steerAngleMax: 0.45,  // radians of front wheel turn at a standstill (full lock)
  steerResponse: 20,  // 1/s: how quickly the wheels follow the steering input
  tyreSurfaceFrictionEffectFactor: 1,  // how much the surface's WHEEL_FRICTION (grass 0.8, slippery rock 0, road 1) counts: 0 none, 1 as the game does
  tyreLoadDistributionSensitivity: 1,  // how the tyres share the weight of the car: 0: equally; 1: in proportion to what their suspensions carry (weight transfer decides what each tyre can hold)
  tyreLoadScalingSensitivity: 1,  // what the weight shared by the tyres adds up to: 0: the weight of the car (downforce and bumps add no grip); 1: what the suspensions really carry
  tyreRadiusFromFile: 1,  // 1: the wheel radii of the car's files (front and rear differ); 0: measured from the wheel meshes
} as const satisfies Partial<ConfigPartCarType>;

type ConfigForCarKeysWithDefaults = keyof ConfigPartCarType & keyof typeof CAR_DEFAULTS_COMMON;
type ConfigForCarKeysWithoutDefaults = Exclude<keyof ConfigPartCarType, ConfigForCarKeysWithDefaults>;

/**
 * What each car's own values start as ("cars" in the JSON). Every car has the same keys. null: not set here: the value of
 * "carDefaults" when the key is there, else the value from the car's files when they have one (mass, spring, damper coefficient).
 */
type ConfigSpecificCarType = ConfigPartial<Pick<ConfigPartCarType, ConfigForCarKeysWithDefaults>> & Pick<ConfigPartCarType, ConfigForCarKeysWithoutDefaults>;
export const CAR_DEFAULTS_PER_CAR: Record<CarId, ConfigSpecificCarType> = {
  eagle: {
    afterburnerForce: 36000,  // N: about three times the car's weight, in the direction the car faces
    bumpStopZone: 0.03,
    centreOfMassFromFileEnabled: 0,  // 1: use the centre of mass of the car's files when it has one; 0: the two numbers centreOfMassHeight, centreOfMassFromRearAxle
    centreOfMassFromFileLoweringAbsolute: 0,  // m: the centre of mass of the car's files is moved down by this much (0 = where the file says)
    centreOfMassFromFileLoweringFraction: 0.2,  // the centre of mass is lowered by this part of the height of the car's hull
    centreOfMassFromRearAxle: 0.60,  // between the axles when not taken from the files: 0 = on the rear axle, 1 = on the front axle
    centreOfMassHeight: 0.8,  // m above the road, when not taken from the car's files
    downforceKnee: 40,  // m/s: below about this speed the downforce grows like the square of the speed, above it in proportion; smooth in between
    downforceRate: 90,  // N per m/s at high speed (the slope the downforce settles to): presses the car onto the road, along its down axis
    drag: 0.25,  // 0.5 * rho * Cd * A: air drag force is drag * speed^2, against the velocity
    engineForce: 24000,  // N at the wheels at low speed (over three times the car's weight)
    enginePower: 252000,  // W: above the speed where force * speed reaches this, the force falls off as 1 / speed
    engineTaperLength: 15,  // ...completely over this many m/s more
    engineTaperStartSpeed: 55,  // m/s: from here the engine's push fades out...
    gravity: 9.81,  // m/s^2
    grip: 1.5,  // friction coefficient on the road: a tyre pushes with at most this times its load
    handbrakeAllWheels: 0,
    hullMiddleSphereScale: 1,  // the three hull spheres in the middle of the centre column are this many times wider
    hullRearSphereScale: 1,  // the three hull spheres at the rear are this many times wider (for a spoiler)
    hullSphereScale: 1.6,  // the hull spheres are this many times the size that fits the car (their bottoms stay at the hull's bottom)
    kangarooSpeed: 8,  // m/s added upwards per jump (about 3 m high from rest)
    massDefault: 1250,  // kg: used when the mass is not taken from the car's files (massFromFile 0, or the files do not say)
    massFromFile: 1,  // 1: the mass of the car's game files, when they have one; 0: mass
    slipFloorSpeed: 3.5,  // m/s: the slip is measured against at least this speed, so it stays small near a standstill
    suspensionLengthMax: 0.4,  // m: fully extended (the wheel hangs in the air)
    suspensionLengthAtRest: 0.2,  // m: the spring length the car stands on at rest (its weight compresses the spring to this)
    suspensionLengthInLoadedModel: 0.3,  // m: the spring length the wheel positions of the car's model files correspond to (equal to suspensionLengthAtRest: the wheels are where the model has them at rest)
    tyreLoadDistributionSensitivity: 0,
    tyreLoadScalingSensitivity: 0,
  },
  rcBandit: {
    afterburnerForce: 20,  // N: about three times its weight
    bumpStopZone: 0.005,
    cameraScale: 2.0,  // the toy car is watched from farther away, relative to its size
    centreOfMassFromFileEnabled: 0,
    centreOfMassFromFileLoweringAbsolute: 0,
    centreOfMassFromFileLoweringFraction: 0,
    centreOfMassFromRearAxle: 0.36,
    centreOfMassHeight: 0.03,  // m above the road
    suspensionDamperCoefficient: 20,  // N s/m: used when suspensionDamperRatioToCritical is 0
    downforceKnee: 10.0,  // m/s (50 km/h)
    downforceRate: 0.50,  // N per m/s: about one car weight at 50 km/h
    drag: 0.01,  // air drag force is this * speed^2 (0.5 * rho * Cd * A)
    engineForce: 10,  // N at the wheels at low speed
    enginePower: 60,  // W
    engineTaperLength: 3,
    engineTaperStartSpeed: 8,
    gravity: 5,
    grip: 1.5,
    hullMiddleSphereScale: 1,
    hullRearSphereScale: 1,  // for the spoiler
    hullSphereScale: 1.6,
    kangarooSpeed: 2.4,  // m/s added upwards per jump (about 30 cm high)
    massDefault: 2,  // kg
    massFromFile: 0,  // the mass above, whatever the car's files say
    slipFloorSpeed: 0.5,
    steerAngleCentrifugalLimitFactor: 0.5,
    suspensionLengthMax: 0.05,  // m: the travel
    suspensionLengthAtRest: 0.03,
    suspensionLengthInLoadedModel: 0.03,
    suspensionSpringK: 330,  // N/m (the Re-Volt files have none)
    tyreLoadDistributionSensitivity: 0.9,
    tyreLoadScalingSensitivity: 0.9,
  },
};
