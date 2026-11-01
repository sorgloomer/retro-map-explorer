// The configuration manager: the one place that knows the configuration of the driving minigame.
//   - the defaults live in defaults.ts;
//   - the JSON text the player edits is read and printed here (sorted, with line numbers in the errors);
//   - the physics engine (C++) gets its numbers from here, computed for the car that is about to be driven: `resolveCar` takes what
//     the car's game files say (mass, spring, wheel radii, centre of mass, ...), applies the configuration to it and returns the
//     single object of final values. Where a file value can be overridden, the decision is made here and only here.
import {
  CAR_DEFAULTS_PER_CAR,
  CAR_DEFAULTS,
  GLOBAL_DEFAULTS,
} from './defaults.ts';
import {sum} from "../utils/math.ts";
import {prettyJson} from "./configjson.ts";
import {
  type CarId,
  CAR_IDS,
  type ConfigPartCarType,
  type ConfigPartial,
  ConfigSchema,
  isCarParameterName, ConfigPartCarSchema, type ConfigType, type ConfigGlobalType, ConfigGlobalSchema
} from "./config.schema.ts";
import {getOwn, isPlainObject, isSubscriptable} from "../utils/objects.ts";
import * as z from "zod/mini";
import {isNullish} from "../utils/nullish.ts";
import type {EngineSettings} from "./config.cpp.ts";

export type { CarId };
export { CAR_IDS };

/** What the game files of a car say, as the loader found it (all lengths in metres, in the car's space: x right, y up, z forward). */
export interface GameFileCar {
  toy: number;                       // 1: a toy car, modelled in real metres (the Re-Volt RC cars)
  mass: number;                      // kg; 0: the files do not say
  suspensionSpringK: number;                   // N/m; 0: not said
  damper: number;                    // N s/m; 0: not said
  wheelRadiusFile: number[];         // per wheel (front left, front right, rear left, rear right); 0: not said
  wheelRadiusMesh: number[];         // measured from the wheel meshes; 0: not measured
  wheelRadiusFallback: number;       // when nothing else is known
  hasCentreOfMass: number;           // 1 when `centreOfMass` is the files' own
  centreOfMass: number[];            // x, y, z from the car's origin
  hasHullBox: number;                // 1 when the files have a collision hull
  hullMin: number[];
  hullMax: number[];
  boundsMin: number[];               // of the chassis mesh
  boundsMax: number[];
  wheelPos: number[][];              // per wheel: the rest position of its centre
}

const CAR_LENGTH_REFERENCE = 4.3;  // m: the car the lengths and speeds of the configuration are set for

export class ConfigError extends Error {}

/** Later arguments are stronger in precedence */
export const mergeShallowConfigObjects = (...args: (Record<string, unknown> | null | undefined)[]): Record<string, unknown> => {
  const result: Record<string, unknown> = {};
  for (const arg of args) {
    if (isSubscriptable(arg)) {
      for (const key of Object.keys(arg)) {
        const value = arg[key];
        if (value !== null && value !== undefined) {
          result[key] = value;
        }
      }
    }
  }
  return result;
}

const getDefaultConfig = (): ConfigType => ({
  ...GLOBAL_DEFAULTS,
  carDefaults: CAR_DEFAULTS,
  cars: CAR_DEFAULTS_PER_CAR,
});

export class ConfigManager {
  // What is configured now. A value that the text leaves out has its default.
  private configObject: ConfigType = getDefaultConfig();

  /** The configuration as JSON text; the defaults when `defaults` is set. Every object is sorted alphabetically. */
  getText(defaults = false): string {
    const root = defaults
      ? { ...GLOBAL_DEFAULTS, carDefaults: { ...CAR_DEFAULTS }, cars: CAR_DEFAULTS_PER_CAR }
      : this.configObject;
    return prettyJson(root);
  }

  /** Replaces the configuration with the one in the text (what it leaves out gets its default). '' when it was applied, else the reason. */
  applyText(text: string): void {
    try {
      const unvalidated = JSON.parse(text);
      const root = z.parse(ConfigSchema, unvalidated);
      for (const carId of CAR_IDS) {
        z.parse(ConfigPartCarSchema, mergeShallowConfigObjects(root.carDefaults, root.cars?.[carId]));
      }
      this.configObject = root;
    } catch (e) {
      throw new ConfigError(e instanceof Error ? e.message : String(e));
    }
  }

  // ---- what the engine gets

  /**
   * The settings of the car that is about to be driven. `file` is what its game files say; the configuration decides what is used:
   * a value of the car that is set wins, else the one of "carDefaults" when it has that key, else the car's file, else the fallback.
   */
  resolveCar(id: CarId, file: GameFileCar): EngineSettings {
    const g = this.configObject;
    const carConfigSelected = g.cars[id] as Partial<ConfigPartCarType>;
    const carConfigFallback = g.carDefaults as Partial<ConfigPartCarType>;
    const carConfigSelectedDefault = CAR_DEFAULTS_PER_CAR[id] as Partial<ConfigPartCarType>;
    const carConfigFallbackDefault = CAR_DEFAULTS as Partial<ConfigPartCarType>;
    // the value of a key of the car: its own, else the common one, else the built-in one (null: nothing set: the caller looks further)
    const value = <K extends keyof ConfigPartCarType>(key: K): ConfigPartCarType[K] | undefined => (
      getOwn(carConfigSelected, key)
      ?? getOwn(carConfigFallback, key)
      ?? getOwn(carConfigSelectedDefault, key)
      ?? getOwn(carConfigFallbackDefault, key)
      ?? undefined
    );
    const need = <K extends keyof ConfigPartCarType>(key: K): ConfigPartCarType[K] => {
      const v = value(key);
      if (v === undefined) throw new Error(`no value for "${key}" of the car "${id}"`);
      return v;
    };
    const toy = file.toy > 0.5;
    const sizeScale = toy ? Math.max(0.02, (file.boundsMax[2] - file.boundsMin[2]) / CAR_LENGTH_REFERENCE) : 1;

    // mass, spring and damper: the car's files when enabled and they say, else the configured one
    const mass = need('massFromFile') > 0.5 && file.mass > 0 ? file.mass : need('massDefault');
    // the spring constant: the car's files (times the ratio) when enabled and they say, else the configured one
    const suspensionSpringK = need('suspensionSpringKFromFileEnabled') > 0.5 && file.suspensionSpringK > 0
      ? file.suspensionSpringK * need('suspensionSpringKFromFileRatio')
      : need('suspensionSpringK');

    const suspensionDamperCoefficient = (() => {
      const suspensionDamperCoefficientSource = value('suspensionDamperCoefficientSource');

      if (suspensionDamperCoefficientSource === "file" && !isNullish(file?.damper) && file.damper > 0) {
        return file.damper;
      }

      if (suspensionDamperCoefficientSource === "relative" && !isNullish(file?.damper) && file.damper > 0) {
        const damperRatioToCritical = need('suspensionDamperCoefficientAsFractionOfCritical');
        if (damperRatioToCritical > 0) {
          const criticalCoefficient = 2 * Math.sqrt(suspensionSpringK * mass * 0.25);
          return damperRatioToCritical * criticalCoefficient;
        }
      }

      return need('suspensionDamperCoefficient');
    })();

    // wheel radii: the car's files when they say (and the configuration allows it), else the meshes, else the fallback
    const radii = [0, 1, 2, 3].map((i) =>
      need('wheelRadiusFromFile') > 0.5 && file.wheelRadiusFile[i] > 0
        ? file.wheelRadiusFile[i]
        : file.wheelRadiusMesh[i] > 0 ? file.wheelRadiusMesh[i] : file.wheelRadiusFallback);
    const wheelR = sum(radii) / 4;

    // centre of mass, from the car's origin: the files' own (lowered), else a low point towards the rear axle
    let centreOfMass: number[];
    if (need('centreOfMassFromFileEnabled') > 0.5 && file.hasCentreOfMass > 0.5) {
      const hullHeight = file.hasHullBox > 0.5 ? file.hullMax[1] - file.hullMin[1] : file.boundsMax[1] - file.boundsMin[1];
      const lowering = need('centreOfMassFromFileLoweringAbsolute') + (toy ? 0 : need('centreOfMassFromFileLoweringFraction') * hullHeight);
      centreOfMass = [file.centreOfMass[0], file.centreOfMass[1] - lowering, file.centreOfMass[2]];
    } else {
      const frontZ = (file.wheelPos[0][2] + file.wheelPos[1][2]) / 2, rearZ = (file.wheelPos[2][2] + file.wheelPos[3][2]) / 2;
      const axleY = (file.wheelPos[0][1] + file.wheelPos[1][1] + file.wheelPos[2][1] + file.wheelPos[3][1]) / 4;
      centreOfMass = [0, axleY - wheelR + need('centreOfMassHeight'), rearZ + need('centreOfMassFromRearAxle') * (frontZ - rearZ)];
    }

    const suspensionLengthMax = need('suspensionLengthMax');
    const sz = sizeScale;
    const carScale = sz * need('cameraScale');

    const car: EngineSettings["car"] = {
      toy: toy ? 1 : 0,
      sizeScale: sz,
      mass,
      centreOfMassX: centreOfMass[0],
      centreOfMassY: centreOfMass[1],
      centreOfMassZ: centreOfMass[2],
      engineForce: need('engineForce'),
      enginePower: need('enginePower'),
      gravity: need('gravity'),
      grip: need('grip'),
      drag: need('drag'),
      afterburnerForce: need('afterburnerForce'),
      kangarooSpeed: need('kangarooSpeed'),
      suspensionSpringK,
      suspensionDamperCoefficient,
      suspensionLengthMax,
      suspensionLengthAtRest: need('suspensionLengthAtRest'),
      suspensionLengthInLoadedModel: need('suspensionLengthInLoadedModel'),
      bumpStopZone: need('bumpStopZone'),
      rayLift: g.suspensionRayLift * wheelR,  // (the setting is in wheel radii)
      wheelRadius0: radii[0], wheelRadius1: radii[1], wheelRadius2: radii[2], wheelRadius3: radii[3],
      steerAngleCentrifugalKneeFactor: need('steerAngleCentrifugalKneeFactor'),
      steerAngleCentrifugalLimitFactor: need('steerAngleCentrifugalLimitFactor'),
      steerAngleMax: need('steerAngleMax'),
      steerResponse: need('steerResponse'),
      tyreLoadDistributionSensitivity: need('tyreLoadDistributionSensitivity'),
      tyreLoadScalingSensitivity: need('tyreLoadScalingSensitivity'),
      hullProbeRadius: need('hullProbeRadius'),
      reverseForceFraction: need('reverseForceFraction'),
      reversePowerFraction: need('reversePowerFraction'),
      handbrakeBrake: need('handbrakeBrake'),
      handbrakeGripLoss: need('handbrakeGripLoss'),
      tyreSurfaceFrictionEffectFactor: need('tyreSurfaceFrictionEffectFactor'),
      driftSlipDeadZone: need('driftSlipDeadZone'),
      driftSlipRange: need('driftSlipRange'),
      driftPointsFactor: need('driftPointsFactor'),
      driftMinSpeed: need('driftMinSpeed') * sz,  // (set for a car of 4.3 m: it follows the size of the car)
      engineTaperStartSpeed: need('engineTaperStartSpeed'),
      engineTaperLength: need('engineTaperLength'),
      slipFloorSpeed: need('slipFloorSpeed'),
      downforceRate: need('downforceRate'),
      downforceKnee: need('downforceKnee'),
      downforceHeightCompensation: need('downforceHeightCompensation'),
      hullWallBounce: need('hullWallBounce'),
      handbrakeAllWheels: need('handbrakeAllWheels'),
      hullSphereScale: need('hullSphereScale'),
      hullMiddleSphereScale: need('hullMiddleSphereScale'),
      hullRearSphereScale: need('hullRearSphereScale'),
      // the speeds and lengths that are set for a car of 4.3 m follow the size of the car
      hullMaxPushOut: g.hullMaxPushOut * sz,
      pickupRadius: g.pickupRadius * sz,
      pickupReachY: g.pickupReachY * sz,
      forceArrowLength: g.forceArrowLength * sz,
      cameraDropScale: Math.max(sz, 0.25),  // the heights the ground is looked for from
      cameraDistance: g.cameraDistance * carScale,
      cameraHeight: g.cameraHeight * carScale,
      cameraFarDistance: g.cameraFarDistance * carScale,
      cameraFarHeight: g.cameraFarHeight * carScale,
      cameraLookAhead: g.cameraLookAhead * sz,
      cameraLookHeight: g.cameraLookHeight * sz,
      cameraWallMargin: g.cameraWallMargin * sz,
      cameraMinDistance: 0.6 * sz,
      cameraWallCheckLength: 0.5 * sz,
      cameraSlideMinSpeed: g.cameraSlideMinSpeed * sz,
      cameraSpeedCap: g.cameraSpeedCap * sz,
    };
    const globals: EngineSettings["globals"] = {
      rollInertiaFactor: g.rollInertiaFactor,
      maxPhysicsStep: g.maxPhysicsStep,
      vehicleMaxFrameSeconds: g.vehicleMaxFrameSeconds,
      bottomOutPushOut: g.bottomOutPushOut,
      bumpStopStiffness: g.bumpStopStiffness,
      tyreSlipCurve: g.tyreSlipCurve,
      tyreSlipKnee: g.tyreSlipKnee,
      tyreSlipKneeValue: g.tyreSlipKneeValue,
      tyreSlipFull: g.tyreSlipFull,
      tyreStopFraction: g.tyreStopFraction,
      slideGrip: g.slideGrip,
      brakeForce: g.brakeForce,
      rollResist: g.rollResist,
      maxSpeed: g.maxSpeed,
      rollCentre: g.rollCentre,
      controlMinPedal: g.controlMinPedal,
      lateralStiffness: g.lateralStiffness,
      airAngularDamping: g.airAngularDamping,
      groundAngularDamping: g.groundAngularDamping,
      floorNormalY: g.floorNormalY,
      floorFriction: g.floorFriction,
      wallFriction: g.wallFriction,
      hullPushOut: g.hullPushOut,
      roundSeconds: g.roundSeconds,
      pointsPowerup: g.pointsPowerup,
      pointsNotable: g.pointsNotable,
      pointsTimeBonus: g.pointsTimeBonus,
      timeBonusSeconds: g.timeBonusSeconds,
      pointsUpgrade: g.pointsUpgrade,
      pointsCredit: g.pointsCredit,
      pointsPickup: g.pointsPickup,
      pointsStar: g.pointsStar,
      finishBonusPerSecond: g.finishBonusPerSecond,
      maxFrameSeconds: g.maxFrameSeconds,
      safeSpotSeconds: g.safeSpotSeconds,
      worldFloorBelow: g.worldFloorBelow,
      cameraSpeedDistance: g.cameraSpeedDistance,
      cameraYawFollow: g.cameraYawFollow,
      cameraPositionFollow: g.cameraPositionFollow,
      cameraPositionDampingRatio: g.cameraPositionDampingRatio,
      cameraSlideBlend: g.cameraSlideBlend,
      keySteerRise: g.keySteerRise,
      keySteerReturn: g.keySteerReturn,
      keySteerReverse: g.keySteerReverse,
      keyAccelRise: g.keyAccelRise,
      keyAccelFall: g.keyAccelFall,
      keyBrakeRise: g.keyBrakeRise,
      keyBrakeFall: g.keyBrakeFall,
      gamepadDeadzone: g.gamepadDeadzone,
    };
    return { globals, car };
  }
}

/** The one configuration of the page. */
export const config = new ConfigManager();
