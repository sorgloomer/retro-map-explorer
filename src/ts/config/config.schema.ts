import * as z from "zod/mini";
import {hasOwn} from "../utils/objects.ts";

/** The car configurations that exist, in the order of the car selector. */
export const CAR_IDS = ['eagle', 'rcBandit'] as const;
export type CarId = (typeof CAR_IDS)[number];

export type ConfigPartCarType = z.infer<typeof ConfigPartCarSchema>;
export type ConfigPartCarTypeKeys = keyof ConfigPartCarType;

export type ConfigPartial<T> = { [K in keyof T]?: T[K] | null | undefined };

export const ConfigGlobalSchema = z.strictObject({
  rollInertiaFactor: z.number(),  // turns the roll (about the forward axis) inertia up or down
  // --- world and time step
  maxPhysicsStep: z.number(),  // s: a frame is split into physics steps of at most this (1/150)
  vehicleMaxFrameSeconds: z.number(),  // s: the longest frame the vehicle simulates in one call
  // --- suspension: lengths from the upper mount down the car's up axis
  bottomOutPushOut: z.number(),  // a wheel past the end of its travel: this part of the overlap is undone each step, without any speed (no bounce)
  bumpStopStiffness: z.number(),  // ...by this factor
  // --- suspension, tyres, engine and air
  tyreSlipCurve: z.number(),  // 1: sideways force follows the original game's slip curve; 0: the older stiffness model
  tyreSlipKnee: z.number(),  // slip (fraction of the speed) at which the curve is at tyreSlipKneeValue of the peak grip
  tyreSlipKneeValue: z.number(),  // part of the peak grip given at the knee
  tyreSlipFull: z.number(),  // slip at which the tyre gives its full grip
  suspensionRayLift: z.number(),  // wheel radii: the wheel rays start this far above the suspension's upper mount, so a curb or a slope that is higher than the mount is still found
  tyreStopFraction: z.number(),  // a tyre asks for at most this part of the force that would cancel its contact speed within one step (the other wheels push too: 1 would overshoot and shake the car)
  slideGrip: z.number(),  // part of the grip left while a tyre slides sideways
  brakeForce: z.number(),  // brake pedal fully down asks for this part of the friction limit
  rollResist: z.number(),  // rolling resistance, as a fraction of the tyre load
  maxSpeed: z.number(),  // m/s (hard limit)
  // --- steering
  // --- driving details
  controlMinPedal: z.number(),  // a pedal below this counts as released
  lateralStiffness: z.number(),  // how much of the sideways speed a tyre tries to cancel in one step (below the limit)
  // --- rotation damping (1/s): the spin loses this part of its speed every second
  angularDampingAir: z.number(),  // no wheel touches the road
  angularDampingGround: z.number(),  // at least one wheel does
  // --- hull contacts (the chassis against the level)
  floorNormalY: z.number(),  // a contact whose normal points up at least this much counts as floor, else as a wall
  floorFriction: z.number(),  // sliding friction on a floor contact, as a multiple of the normal impulse
  wallFriction: z.number(),  // and on a wall contact
  hullPushOut: z.number(),  // how much of the penetration depth is corrected in one step...
  hullMaxPushOut: z.number(),  // ...but never more than this many metres
  // --- rules
  roundSeconds: z.number(),  // s on the clock in Powerup Rush
  pickupRadius: z.number(),  // m: a powerup is collected this close to the car (horizontally)...
  pickupReachY: z.number(),  // ...and this close vertically
  pointsPowerup: z.number(),  // score of an ordinary powerup (also pedestrian ones)
  pointsNotable: z.number(),  // notable powerups and mission items
  pointsTimeBonus: z.number(),  // time bonus powerups, which also add timeBonusSeconds
  timeBonusSeconds: z.number(),
  pointsUpgrade: z.number(),
  pointsCredit: z.number(),
  pointsPickup: z.number(),
  pointsStar: z.number(),
  finishBonusPerSecond: z.number(),  // score per second left when every powerup is collected
  maxFrameSeconds: z.number(),  // s: the longest frame the game simulates in one call
  safeSpotSeconds: z.number(),  // s between remembering a safe spot to return to after falling out of the world
  worldFloorBelow: z.number(),  // m below the level's lowest point: a car below it fell out of the world
  // --- camera
  cameraDistance: z.number(),  // m behind the car (chase camera)...
  cameraHeight: z.number(),  // ...and above it
  cameraFarDistance: z.number(),  // the same for the far camera (C)
  cameraFarHeight: z.number(),
  forceArrowLength: z.number(),  // debug arrows of the wheel forces: metres per car weight (a wheel carries a quarter of it at rest)
  cameraSpeedDistance: z.number(),  // m of extra distance per m/s of speed...
  cameraSpeedCap: z.number(),  // ...up to this speed
  cameraYawFollow: z.number(),  // 1/s: how quickly the camera turns behind the car
  cameraPositionFollow: z.number(),  // 1/s: how quickly it follows the wanted position (the natural frequency of the camera's spring)
  cameraPositionDampingRatio: z.number(),  // damping of that spring: 1 settles fastest without swinging past; below 1 the camera overshoots a little (more inertia)
  cameraLookAhead: z.number(),  // m ahead of the car that it looks at
  cameraLookHeight: z.number(),  // m above the car that it looks at
  cameraSlideMinSpeed: z.number(),  // m/s: faster than this, the camera looks where the car goes when it slides...
  cameraSlideBlend: z.number(),  // ...by this part of the difference between heading and movement
  cameraWallMargin: z.number(),  // m kept between the camera and a wall behind the car
  // --- keyboard feel (1/s: how fast the digital keys move the analog value)
  keySteerRise: z.number(),  // steering towards a key that is held
  keySteerReturn: z.number(),  // steering back to the centre when no key is held
  keySteerReverse: z.number(),  // steering from one side to the other
  keyAccelRise: z.number(),
  keyAccelFall: z.number(),
  keyBrakeRise: z.number(),
  keyBrakeFall: z.number(),
  gamepadDeadzone: z.number(),  // the stick's dead zone, 0..1
});

export type ConfigGlobalType = z.infer<typeof ConfigGlobalSchema>;

export const ConfigPartCarSchema = z.strictObject({
  afterburnerForce: z.number(),
  bumpStopZone: z.number(),
  cameraScale: z.number(),
  centreOfMassFromFileEnabled: z.number(),
  centreOfMassFromFileLoweringAbsolute: z.number(),
  centreOfMassFromFileLoweringFraction: z.number(),
  centreOfMassFromRearAxle: z.number(),
  centreOfMassHeight: z.number(),
  downforceHeightCompensation: z.number(),
  downforceKnee: z.number(),
  downforceRate: z.number(),
  drag: z.number(),
  driftMinSpeed: z.number(),
  driftPointsFactor: z.number(),
  driftSlipDeadZone: z.number(),
  driftSlipRange: z.number(),
  engineForce: z.number(),
  enginePower: z.number(),
  engineTaperLength: z.number(),
  engineTaperStartSpeed: z.number(),
  gravity: z.number(),
  grip: z.number(),
  handbrakeAllWheels: z.number(),
  handbrakeBrake: z.number(),
  handbrakeGripLoss: z.number(),
  hullMiddleSphereScale: z.number(),
  hullProbeRadius: z.number(),
  hullRearSphereScale: z.number(),
  hullSphereScale: z.number(),
  hullWallBounce: z.number(),
  kangarooSpeed: z.number(),
  massDefault: z.number(),
  massFromFile: z.number(),
  rendererCarBrightness: z.number(),
  reverseForceFraction: z.number(),
  reversePowerFraction: z.number(),
  slipFloorSpeed: z.number(),
  steerAngleCentrifugalKneeFactor: z.number(),
  steerAngleCentrifugalLimitFactor: z.number(),
  steerAngleMax: z.number(),
  steerResponse: z.number(),
  suspensionDamperCoefficientSource: z.enum(["file", "relative", "absolute"]),
  suspensionDamperCoefficientAsFractionOfCritical: z.number(),
  suspensionDamperCoefficient: z.number(),
  suspensionLengthAtRest: z.number(),
  suspensionLengthInLoadedModel: z.number(),
  suspensionLengthMax: z.number(),
  suspensionSpringK: z.number(),
  suspensionSpringKFromFileEnabled: z.number(),
  suspensionSpringKFromFileRatio: z.number(),
  tyreForceLiftOffset: z.number(),
  tyreForceLiftRadiusFraction: z.number(),
  tyreLoadDistributionSensitivity: z.number(),
  tyreLoadScalingSensitivity: z.number(),
  tyreSurfaceFrictionEffectFactor: z.number(),
  tyreRadiusFromFile: z.number(),
});

type ConfigPartialShape<S extends z.core.$ZodShape> = {
  [K in keyof S]: z.ZodMiniOptional<z.ZodMiniNullable<S[K]>>;
};

export const configPartial = <S extends z.core.$ZodShape>(
  schema: z.ZodMiniObject<S, z.core.$ZodObjectConfig>
): z.ZodMiniObject<ConfigPartialShape<S>, z.core.$strict> => {
  return z.strictObject(
    Object.fromEntries(
      Object.entries(schema.shape).map(([key, value]) => [
        key,
        z.optional(z.nullable(value)),
      ])
    ) as ConfigPartialShape<S>
  );
};

/** True for the name of a setting of a car (every setting has a value in CAR_DEFAULTS or in the list of a car). */
export const isCarParameterName = (name: string): name is ConfigPartCarTypeKeys => hasOwn(ConfigPartCarSchema.shape, name);


const buildRecord = <Ks extends ReadonlyArray<PropertyKey>, V>(keys: Ks, fn: (k: Ks[number]) => V) => Object.fromEntries(keys.map(key => [key, fn(key)])) as { [K in Ks[number]]: V };
const ConfigPartCarCollectionSchema = configPartial(z.strictObject(
  buildRecord(CAR_IDS, () => configPartial(ConfigPartCarSchema))
));

export const ConfigSchema = z.strictObject({
  ...ConfigGlobalSchema.shape,
  carDefaults: configPartial(ConfigPartCarSchema),
  cars: ConfigPartCarCollectionSchema,
});

export type ConfigType = z.infer<typeof ConfigSchema>;
