// The configuration manager: the one place that knows the configuration of the driving minigame.
//   - the defaults live in defaults.ts;
//   - the JSON text the player edits is read and printed here (sorted, with line numbers in the errors);
//   - the physics engine (C++) gets its numbers from here, computed for the car that is about to be driven: `resolveCar` takes what
//     the car's game files say (mass, spring, wheel radii, centre of mass, ...), applies the configuration to it and returns the
//     single object of final values. Where a file value can be overridden, the decision is made here and only here.
import { CAR_BUILTIN, CAR_DEFAULTS, CAR_IDS, GLOBAL_DEFAULTS } from './defaults.ts';
import type { CarId } from './defaults.ts';

export type { CarId };
export { CAR_IDS };

/** What the game files of a car say, as the loader found it (all lengths in metres, in the car's space: x right, y up, z forward). */
export interface GameFileCar {
  toy: number;                       // 1: a toy car, modelled in real metres (the Re-Volt RC cars)
  mass: number;                      // kg; 0: the files do not say
  springK: number;                   // N/m; 0: not said
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
  'suspensionRayLift', 'suspensionDampingRatio', 'powerMinSpeed', 'reversePowerMinSpeed', 'driftMinSpeed', 'hullMaxPushOut', 'stuckMaxSpeed',
  'pickupRadius', 'pickupReachY', 'cameraDistance', 'cameraHeight', 'cameraFarDistance', 'cameraFarHeight', 'cameraLookAhead', 'cameraLookHeight',
  'cameraWallMargin', 'cameraSlideMinSpeed', 'cameraSpeedCap', 'forceArrowLength',
]);

const FALLBACK_SPRING_K = 30000;  // N/m, when neither the configuration nor the car's files say
const FALLBACK_DAMPER = 3300;     // N s/m
const CAR_LENGTH_REFERENCE = 4.3;  // m: the car the lengths and speeds of the configuration are set for

class ConfigError extends Error {
  readonly line: number;
  constructor(line: number, message: string) {
    super(message);
    this.line = line;
  }
}

// ---- reading the text

type Node = { kind: 'object'; members: { name: string; line: number; value: Node }[] } | { kind: 'number'; value: number } | { kind: 'null' };

class Reader {
  pos = 0;
  line = 1;
  readonly text: string;
  constructor(text: string) {
    this.text = text;
  }

  fail(message: string): never {
    throw new ConfigError(this.line, message);
  }
  skip(): void {
    while (this.pos < this.text.length && /\s/.test(this.text[this.pos])) {
      if (this.text[this.pos] === '\n') this.line++;
      this.pos++;
    }
  }
  value(): Node {
    this.skip();
    const c = this.text[this.pos];
    if (c === '{') return this.object();
    if (this.text.startsWith('null', this.pos)) {
      this.pos += 4;
      return { kind: 'null' };
    }
    const m = /^-?\d+(\.\d+)?([eE][+-]?\d+)?/.exec(this.text.slice(this.pos));
    if (!m) return this.fail('expected a number, null or an object');
    this.pos += m[0].length;
    return { kind: 'number', value: Number(m[0]) };
  }
  object(): Node {
    this.pos++;  // {
    const members: { name: string; line: number; value: Node }[] = [];
    let first = true;
    for (;;) {
      this.skip();
      if (this.pos >= this.text.length) return this.fail("the object is not closed, expected '}'");
      if (this.text[this.pos] === '}') {
        this.pos++;
        return { kind: 'object', members };
      }
      if (!first) {
        if (this.text[this.pos] !== ',') return this.fail("expected ',' or '}'");
        this.pos++;
        this.skip();
        if (this.text[this.pos] === '}') return this.fail("a ',' before '}' is not allowed");
      }
      first = false;
      if (this.text[this.pos] !== '"') return this.fail('expected a "name"');
      const end = this.text.indexOf('"', this.pos + 1);
      if (end < 0) return this.fail('the name is not closed');
      const name = this.text.slice(this.pos + 1, end);
      const line = this.line;
      this.pos = end + 1;
      this.skip();
      if (this.text[this.pos] !== ':') return this.fail(`expected ':' after "${name}"`);
      this.pos++;
      members.push({ name, line, value: this.value() });
    }
  }
}

/** Alphabetical, ignoring the case. */
const before = (a: string, b: string): number => {
  const x = a.toLowerCase(), y = b.toLowerCase();
  return x < y ? -1 : x > y ? 1 : a < b ? -1 : a > b ? 1 : 0;
};

const numberText = (v: number | null): string => (v === null ? 'null' : String(v));

export class ConfigManager {
  // What is configured now. A value that the text leaves out has its default.
  private globals: Record<string, number> = { ...GLOBAL_DEFAULTS };
  private carDefaults: Record<string, number> = { ...CAR_DEFAULTS };
  private cars: Record<CarId, Record<string, number | null>> = { eagle: { ...CAR_BUILTIN.eagle }, rcBandit: { ...CAR_BUILTIN.rcBandit } };

  /** The configuration as JSON text; the defaults when `defaults` is set. Every object is sorted alphabetically. */
  text(defaults = false): string {
    const g = defaults ? GLOBAL_DEFAULTS : this.globals;
    const d = defaults ? CAR_DEFAULTS : this.carDefaults;
    const c = defaults ? CAR_BUILTIN : this.cars;
    const lines: string[] = [];
    const entries: { key: string; text: string }[] = Object.keys(g).map((key) => ({ key, text: `  "${key}": ${numberText(g[key])}` }));
    const object = (indent: string, values: Record<string, number | null>): string => {
      const keys = Object.keys(values).sort(before);
      return '{\n' + keys.map((k) => `${indent}  "${k}": ${numberText(values[k])}`).join(',\n') + `\n${indent}}`;
    };
    entries.push({ key: 'carDefaults', text: `  "carDefaults": ${object('  ', d)}` });
    const carTexts = [...CAR_IDS].sort(before).map((id) => `    "${id}": ${object('    ', c[id])}`);
    entries.push({ key: 'cars', text: `  "cars": {\n${carTexts.join(',\n')}\n  }` });
    entries.sort((a, b) => before(a.key, b.key));
    lines.push(entries.map((e) => e.text).join(',\n'));
    return `{\n${lines.join('')}\n}\n`;
  }

  /** Replaces the configuration with the one in the text (what it leaves out gets its default). '' when it was applied, else the reason. */
  apply(text: string): string {
    try {
      const reader = new Reader(text);
      reader.skip();
      if (reader.text[reader.pos] !== '{') reader.fail("expected '{'");
      const root = reader.object();
      reader.skip();
      if (reader.pos < text.length) reader.fail("unexpected text after the closing '}'");
      const globals = { ...GLOBAL_DEFAULTS };
      const carDefaults = { ...CAR_DEFAULTS };
      const cars: Record<CarId, Record<string, number | null>> = { eagle: { ...CAR_BUILTIN.eagle }, rcBandit: { ...CAR_BUILTIN.rcBandit } };
      if (root.kind !== 'object') throw new ConfigError(1, 'expected an object');
      for (const m of root.members) {
        if (m.name === 'carDefaults') {
          this.fill(carDefaults, m.value, m.line, 'carDefaults', false);
        } else if (m.name === 'cars') {
          if (m.value.kind !== 'object') throw new ConfigError(m.line, '"cars" must be an object');
          for (const car of m.value.members) {
            if (!(CAR_IDS as readonly string[]).includes(car.name)) throw new ConfigError(car.line, `unknown car "${car.name}"`);
            this.fill(cars[car.name as CarId], car.value, car.line, `the car "${car.name}"`, true);
          }
        } else if (m.name in GLOBAL_DEFAULTS) {
          if (m.value.kind !== 'number') throw new ConfigError(m.line, `"${m.name}" must be a number`);
          globals[m.name] = m.value.value;
        } else {
          throw new ConfigError(m.line, `unknown parameter "${m.name}"`);
        }
      }
      this.globals = globals;
      this.carDefaults = carDefaults;
      this.cars = cars;
      return '';
    } catch (e) {
      if (e instanceof ConfigError) return `line ${e.line}: ${e.message}`;
      throw e;
    }
  }

  // The members of an object into `target`: only the keys that `target` has, numbers (and null, when allowed).
  private fill(target: Record<string, number | null>, node: Node, line: number, where: string, allowNull: boolean): void {
    if (node.kind !== 'object') throw new ConfigError(line, `${where} must be an object`);
    for (const m of node.members) {
      if (!(m.name in target)) throw new ConfigError(m.line, `unknown parameter "${m.name}" of ${where}`);
      if (m.value.kind === 'null') {
        if (!allowNull) throw new ConfigError(m.line, `"${m.name}" must be a number`);
        target[m.name] = null;
      } else if (m.value.kind === 'number') {
        target[m.name] = m.value.value;
      } else {
        throw new ConfigError(m.line, `"${m.name}" must be a number${allowNull ? ' or null' : ''}`);
      }
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
    const value = (key: string): number | null => own[key] ?? this.carDefaults[key] ?? CAR_BUILTIN[id][key] ?? null;
    const need = (key: string): number => {
      const v = value(key);
      if (v === null) throw new Error(`no value for "${key}" of the car "${id}"`);
      return v;
    };
    const toy = file.toy > 0.5;
    const sizeScale = toy ? Math.max(0.02, (file.boundsMax[2] - file.boundsMin[2]) / CAR_LENGTH_REFERENCE) : 1;

    // mass, spring and damper: set, else the car's files, else the fallback
    const mass = value('mass') ?? (file.mass > 0 ? file.mass : need('defaultMass'));
    const springK = value('springK') ?? (file.springK > 0 ? file.springK : FALLBACK_SPRING_K);
    const rawDamper = value('damper') ?? (file.damper > 0 ? file.damper : FALLBACK_DAMPER);
    // the damper from a damping ratio when asked for: 1 is the critical damping of the spring that carries a quarter of the car
    const damper = g.suspensionDampingRatio > 0 ? g.suspensionDampingRatio * 2 * Math.sqrt(springK * mass * 0.25) : rawDamper;

    // wheel radii: the car's files when they say (and the configuration allows it), else the meshes, else the fallback
    const radii = [0, 1, 2, 3].map((i) =>
      need('wheelRadiusFromFile') > 0.5 && file.wheelRadiusFile[i] > 0
        ? file.wheelRadiusFile[i]
        : file.wheelRadiusMesh[i] > 0 ? file.wheelRadiusMesh[i] : file.wheelRadiusFallback);
    const wheelR = radii.reduce((a, b) => a + b, 0) / 4;

    // centre of mass, from the car's origin: the files' own (lowered), else a low point towards the rear axle
    let centreOfMass: number[];
    if (need('centreOfMassFromFile') > 0.5 && file.hasCentreOfMass > 0.5) {
      const hullHeight = file.hasHullBox > 0.5 ? file.hullMax[1] - file.hullMin[1] : file.boundsMax[1] - file.boundsMin[1];
      const lowering = need('centreOfMassLowering') + (toy ? 0 : need('centreOfMassLoweringFraction') * hullHeight);
      centreOfMass = [file.centreOfMass[0], file.centreOfMass[1] - lowering, file.centreOfMass[2]];
    } else {
      const frontZ = (file.wheelPos[0][2] + file.wheelPos[1][2]) / 2, rearZ = (file.wheelPos[2][2] + file.wheelPos[3][2]) / 2;
      const axleY = (file.wheelPos[0][1] + file.wheelPos[1][1] + file.wheelPos[2][1] + file.wheelPos[3][1]) / 4;
      centreOfMass = [0, axleY - wheelR + need('centreOfMassHeight'), rearZ + need('centreOfMassFromRearAxle') * (frontZ - rearZ)];
    }

    // the ray length of the suspension follows its travel, against the one of the Eagle's it was set for
    const eagleTravel = this.cars.eagle.suspMax ?? CAR_BUILTIN.eagle.suspMax ?? 0.3;
    const suspMax = need('suspMax');
    const sz = sizeScale;
    const carScale = sz * need('cameraScale');

    const car: Record<string, number> = {
      toy: toy ? 1 : 0,
      sizeScale: sz,
      mass,
      centreOfMassX: centreOfMass[0],
      centreOfMassY: centreOfMass[1],
      centreOfMassZ: centreOfMass[2],
      engineForce: need('engineForce') * need('engineTorqueScale'),
      enginePower: need('enginePower') * need('engineTorqueScale'),
      grip: need('grip'),
      drag: need('drag'),
      afterburnerForce: need('afterburnerForce'),
      kangarooSpeed: need('kangarooSpeed'),
      springK,
      damper,
      suspMin: need('suspMin'),
      suspMax,
      suspRest: need('suspRest'),
      springFree: need('springFree'),
      bumpStopZone: need('bumpStopZone'),
      rayLift: g.suspensionRayLift * (suspMax / Math.max(1e-3, eagleTravel)),
      wheelRadius0: radii[0], wheelRadius1: radii[1], wheelRadius2: radii[2], wheelRadius3: radii[3],
      steerFadeSpeed: need('steerFadeSpeed'),
      taperStartSpeed: need('taperStartSpeed'),
      taperLength: need('taperLength'),
      slipFloorSpeed: need('slipFloorSpeed'),
      downforceRate: need('downforceRate'),
      downforceKnee: need('downforceKnee'),
      downforceHeightCompensation: need('downforceHeightCompensation'),
      sphereScale: need('sphereScale'),
      middleSphereScale: need('middleSphereScale'),
      rearSphereScale: need('rearSphereScale'),
      // the speeds and lengths that are set for a car of 4.3 m follow the size of the car
      powerMinSpeed: g.powerMinSpeed * sz,
      reversePowerMinSpeed: g.reversePowerMinSpeed * sz,
      driftMinSpeed: g.driftMinSpeed * sz,
      hullMaxPushOut: g.hullMaxPushOut * sz,
      stuckMaxSpeed: g.stuckMaxSpeed * sz,
      pickupRadius: g.pickupRadius * sz,
      pickupReachY: g.pickupReachY * sz,
      forceArrowLength: g.forceArrowLength * sz,
      dropScale: Math.max(sz, 0.25),  // the heights the ground is looked for from
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
