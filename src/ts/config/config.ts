// The configuration manager: the one place that knows the configuration of the driving minigame.
//   - the defaults live in defaults.ts;
//   - the JSON text the player edits is read and printed here (sorted, with line numbers in the errors);
//   - the physics engine (C++) gets its numbers from here, computed for the car that is about to be driven: `resolveCar` takes what
//     the car's game files say (mass, spring, wheel radii, centre of mass, ...), applies the configuration to it and returns the
//     single object of final values. Where a file value can be overridden, the decision is made here and only here.
import { CAR_BUILTIN, CAR_DEFAULTS, CAR_IDS, CAR_WORDS, GLOBAL_DEFAULTS, GRIP_MODES } from './defaults.ts';
import type { CarId, CarValue, GripMode } from './defaults.ts';
import {sum} from "../utils/math.ts";

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

/** The numbers the engine gets: `globals` are the same for every car, `car` is computed for the car about to be driven. */
export interface EngineSettings {
  globals: Record<string, number>;
  car: Record<string, number>;
}

// What goes to the engine as it is, and what is only an input of the car's computation (the numbers of the cars that are set for a
// car of 4.3 m and made to fit the size of the car that is driven, ...).
const COMPUTED_FROM_GLOBALS = new Set([
  'suspensionRayLift', 'driftMinSpeed', 'hullMaxPushOut',
  'pickupRadius', 'pickupReachY', 'cameraDistance', 'cameraHeight', 'cameraFarDistance', 'cameraFarHeight', 'cameraLookAhead', 'cameraLookHeight',
  'cameraWallMargin', 'cameraSlideMinSpeed', 'cameraSpeedCap', 'forceArrowLength',
]);

const FALLBACK_DAMPER = 3300;     // N s/m
const CAR_LENGTH_REFERENCE = 4.3;  // m: the car the lengths and speeds of the configuration are set for

class ConfigError extends Error {}

/** Alphabetical, ignoring the case. */
const before = (a: string, b: string): number => {
  const x = a.toLowerCase(), y = b.toLowerCase();
  return x < y ? -1 : x > y ? 1 : a < b ? -1 : a > b ? 1 : 0;
};

/** The same object with its keys in alphabetical order (the order JSON.stringify writes them in). */
const sortedKeys = <T>(values: Record<string, T>): Record<string, T> =>
  Object.fromEntries(Object.keys(values).sort(before).map((key) => [key, values[key]]));

const isObject = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v);

export class ConfigManager {
  // What is configured now. A value that the text leaves out has its default.
  private globals: Record<string, number> = { ...GLOBAL_DEFAULTS };
  private carDefaults: Record<string, CarValue> = { ...CAR_DEFAULTS };
  private cars: Record<CarId, Record<string, CarValue>> = { eagle: { ...CAR_BUILTIN.eagle }, rcBandit: { ...CAR_BUILTIN.rcBandit } };

  /** The configuration as JSON text; the defaults when `defaults` is set. Every object is sorted alphabetically. */
  text(defaults = false): string {
    const cars = defaults ? CAR_BUILTIN : this.cars;
    const root = {
      ...(defaults ? GLOBAL_DEFAULTS : this.globals),
      carDefaults: sortedKeys(defaults ? CAR_DEFAULTS : this.carDefaults),
      cars: sortedKeys(Object.fromEntries(CAR_IDS.map((id) => [id, sortedKeys(cars[id])]))),
    };
    return JSON.stringify(sortedKeys(root), null, 2) + '\n';
  }

  /** Replaces the configuration with the one in the text (what it leaves out gets its default). '' when it was applied, else the reason. */
  apply(text: string): string {
    let root: unknown;
    try {
      root = JSON.parse(text);
    } catch (e) {
      return e instanceof Error ? e.message : String(e);
    }
    try {
      if (!isObject(root)) throw new ConfigError('expected an object');
      const globals = { ...GLOBAL_DEFAULTS };
      const carDefaults: Record<string, CarValue> = { ...CAR_DEFAULTS };
      const cars: Record<CarId, Record<string, CarValue>> = { eagle: { ...CAR_BUILTIN.eagle }, rcBandit: { ...CAR_BUILTIN.rcBandit } };
      for (const [name, value] of Object.entries(root)) {
        if (name === 'carDefaults') {
          this.fill(carDefaults, value, 'carDefaults', false);
        } else if (name === 'cars') {
          if (!isObject(value)) throw new ConfigError('"cars" must be an object');
          for (const [id, carValue] of Object.entries(value)) {
            if (!(CAR_IDS as readonly string[]).includes(id)) throw new ConfigError(`unknown car "${id}"`);
            this.fill(cars[id as CarId], carValue, `the car "${id}"`, true);
          }
        } else if (name in GLOBAL_DEFAULTS) {
          if (typeof value !== 'number') throw new ConfigError(`"${name}" must be a number`);
          globals[name] = value;
        } else {
          throw new ConfigError(`unknown parameter "${name}"`);
        }
      }
      this.globals = globals;
      this.carDefaults = carDefaults;
      this.cars = cars;
      return '';
    } catch (e) {
      if (e instanceof ConfigError) return e.message;
      throw e;
    }
  }

  // The members of an object into `target`: only the keys that `target` has; numbers, or one of the words for the settings that have
  // words (CAR_WORDS), and null when allowed.
  private fill(target: Record<string, CarValue>, node: unknown, where: string, allowNull: boolean): void {
    if (!isObject(node)) throw new ConfigError(`${where} must be an object`);
    for (const [name, value] of Object.entries(node)) {
      if (!(name in target)) throw new ConfigError(`unknown parameter "${name}" of ${where}`);
      const words = CAR_WORDS[name];
      const orNull = allowNull ? ' or null' : '';
      if (value === null) {
        if (!allowNull) throw new ConfigError(`"${name}" of ${where} must be ${words ? 'a word' : 'a number'}`);
      } else if (words) {
        if (typeof value !== 'string' || !words.includes(value)) {
          throw new ConfigError(`"${name}" of ${where} must be one of ${words.map((w) => `"${w}"`).join(', ')}${orNull}`);
        }
      } else if (typeof value !== 'number') {
        throw new ConfigError(`"${name}" of ${where} must be a number${orNull}`);
      }
      target[name] = value;
    }
  }

  // ---- what the engine gets

  /**
   * The settings of the car that is about to be driven. `file` is what its game files say; the configuration decides what is used:
   * a value of the car that is set wins, else the one of "carDefaults" when it has that key, else the car's file, else the fallback.
   */
  resolveCar(id: CarId, file: GameFileCar): EngineSettings {
    const g = this.globals;
    const own = this.cars[id];
    // the value of a key of the car: its own, else the common one, else the built-in one (null: nothing set: the caller looks further)
    const raw = (key: string): CarValue => own[key] ?? this.carDefaults[key] ?? CAR_BUILTIN[id][key] ?? null;
    const value = (key: string): number | null => {
      const v = raw(key);
      if (typeof v === 'string') throw new Error(`"${key}" of the car "${id}" is a word, not a number`);
      return v;
    };
    const need = (key: string): number => {
      const v = value(key);
      if (v === null) throw new Error(`no value for "${key}" of the car "${id}"`);
      return v;
    };
    const needWord = (key: string): string => {
      const v = raw(key);
      if (typeof v !== 'string') throw new Error(`no word for "${key}" of the car "${id}"`);
      return v;
    };
    const toy = file.toy > 0.5;
    const sizeScale = toy ? Math.max(0.02, (file.boundsMax[2] - file.boundsMin[2]) / CAR_LENGTH_REFERENCE) : 1;

    // mass, spring and damper: set, else the car's files, else the fallback
    const mass = value('mass') ?? (file.mass > 0 ? file.mass : need('defaultMass'));
    // the spring constant: the car's files (times the ratio) when enabled and they say, else the configured one
    const suspensionSpringK = need('suspensionSpringKFromFileEnabled') > 0.5 && file.suspensionSpringK > 0
      ? file.suspensionSpringK * need('suspensionSpringKFromFileRatio')
      : need('suspensionSpringK');
    const rawDamperCoefficient = value('suspensionDamperCoefficient') ?? (file.damper > 0 ? file.damper : FALLBACK_DAMPER);
    // the damper from a ratio to the critical damping when asked for: 1 is the critical damping of the spring that carries a quarter of the car
    const damperRatioToCritical = need('suspensionDamperRatioToCritical');
    const suspensionDamperCoefficient = damperRatioToCritical > 0 ? damperRatioToCritical * 2 * Math.sqrt(suspensionSpringK * mass * 0.25) : rawDamperCoefficient;

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

    const car: Record<string, number> = {
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
      steerAngleMax: need('steerAngleMax'),
      steerResponse: need('steerResponse'),
      steerFadeSpeed: need('steerFadeSpeed'),
      engineTaperStartSpeed: need('engineTaperStartSpeed'),
      engineTaperLength: need('engineTaperLength'),
      slipFloorSpeed: need('slipFloorSpeed'),
      downforceRate: need('downforceRate'),
      downforceKnee: need('downforceKnee'),
      downforceHeightCompensation: need('downforceHeightCompensation'),
      hullWallBounce: need('hullWallBounce'),
      handbrakeAllWheels: need('handbrakeAllWheels'),
      gripMode: GRIP_MODES.indexOf(needWord('gripMode') as GripMode),  // the engine gets the position of the word
      hullSphereScale: need('hullSphereScale'),
      hullMiddleSphereScale: need('hullMiddleSphereScale'),
      hullRearSphereScale: need('hullRearSphereScale'),
      // the speeds and lengths that are set for a car of 4.3 m follow the size of the car
      driftMinSpeed: g.driftMinSpeed * sz,
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
    const globals: Record<string, number> = {};
    for (const key of Object.keys(g)) if (!COMPUTED_FROM_GLOBALS.has(key)) globals[key] = g[key];
    return { globals, car };
  }
}

/** The one configuration of the page. */
export const config = new ConfigManager();
