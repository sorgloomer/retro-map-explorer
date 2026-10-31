// The state the components render: one plain object, replaced piece by piece through update(). Components subscribe to the
// parts they use with useUi(selector) and re-render when the selected value changes (compared with Object.is, so nested
// values are replaced, never mutated).
import { useEffect, useRef, useState } from 'preact/hooks';
import { folds, store } from '../storage.ts';
import { OPTS } from './options.ts';

export interface LayerRow {
  id: string;
  name: string;
  color: string;
  count: number;
  visible: boolean;
  idxs: number[];  // category indexes switched by this row (the race line also switches its start marks)
}
export interface LayerGroup { title: string; rows: LayerRow[] }

export interface SceneView {
  title: string;
  info: string;
  canCull: boolean;
  hasCollision: boolean;
  hasPowerupMeshes: boolean;
  hasMovers: boolean;
  canPlay: boolean;
  cars: number;  // bit mask of the cars that can be driven: 1 = the Carmageddon Eagle, 2 = the Re-Volt RC Bandit
  groups: LayerGroup[];
}

export interface GameHud {
  speed: number;
  score: number;
  drift: number;
  pups: string;
  time: string;
  urgent: boolean;   // the countdown turns red
  message: string;
  messageVisible: boolean;
  over: boolean;
  overTitle: string;
  overText: string;
}

export interface LevelOption { id: string; name: string }
export interface LevelGroup { title: string | null; levels: LevelOption[] }

export interface UiState {
  status: { text: string; error: boolean };
  busy: { on: boolean; text: string };
  dropOpen: boolean;       // the "drop your game folder" dialog
  dragging: boolean;       // files are being dragged over the window
  restoreHint: string;
  resumeLabel: string;
  games: { id: string; label: string }[];
  gamesMessage: string;    // shown instead of the games when none was found
  gameId: string;
  levelGroups: LevelGroup[];
  levelId: string;
  scene: SceneView | null;
  camMode: number;         // 0 = flying, 1 = orbital
  touch: boolean;
  dockMin: boolean;
  folds: Record<string, number>;
  opts: Record<string, number>;  // Display options by preference key
  animPaused: boolean;
  animTime: number;
  share: { url: string; msg: string };
  gameMode: boolean;
  hud: GameHud;
  gameDebug: {
    torque: { wheels: number[]; hull: number[]; spin: number[]; toy: number };  // about the centre of mass: kN m (N m * 10^-3 for the toy car, toy = 1) (pitch, yaw, roll) by source, and rad/s
    wheels: { lat: number; limit: number; lon: number; fric: number; slide: number; g: number; slip: number; load: number }[];  // front left, front right, rear left, rear right
    toy: number;
    lights: { inRange: number; max: number; colours: string[] };  // the point lights that reach the car, and how many the shader can use
  };
  car: number;           // the car to drive: 0 = the Carmageddon Eagle, 1 = the Re-Volt RC Bandit (a rookie RC car)
  debugSpheres: boolean;  // the hull's collision spheres are drawn in the game
  debugForces: boolean;  // the wheel force arrows are drawn in the game
  debugPanel: boolean;   // the lateral force panel of the game is shown (not just its title)
  paramsOpen: boolean;   // the minigame parameter editor is open
  tip: { text: string; cat: string; x: number; y: number; swatch: string | null } | null;  // swatch: a CSS colour shown next to the text
}

export const state: UiState = {
  status: { text: '', error: false },
  busy: { on: false, text: 'Loading…' },
  dropOpen: true,
  dragging: false,
  restoreHint: '',
  resumeLabel: '',
  games: [],
  gamesMessage: 'No game loaded',
  gameId: '',
  levelGroups: [],
  levelId: '',
  scene: null,
  camMode: 1,
  touch: false,
  dockMin: false,
  folds: { ...folds },
  opts: Object.fromEntries(OPTS.map((o) => [o.key, o.def])),
  animPaused: false,
  animTime: 0,
  share: { url: '', msg: '' },
  gameMode: false,
  hud: { speed: 0, score: 0, drift: 0, pups: '0 / 0', time: '1:30', urgent: false, message: '', messageVisible: false, over: false, overTitle: '', overText: '' },
  gameDebug: {
    torque: { wheels: [0, 0, 0], hull: [0, 0, 0], spin: [0, 0, 0], toy: 0 },
    wheels: [],
    toy: 0,
    lights: { inRange: 0, max: 0, colours: [] },
  },
  car: store.get('rme.car.v1') === '1' ? 1 : 0,
  debugSpheres: store.get('rme.debugSpheres.v1') === '1',
  debugForces: store.get('rme.debugForces.v1') === '1',
  debugPanel: store.get('rme.debugPanel.v1') === '1',
  paramsOpen: false,
  tip: null,
};

const listeners = new Set<() => void>();

export function update(patch: Partial<UiState>): void {
  Object.assign(state, patch);
  for (const l of [...listeners]) l();
}

/** Re-renders the calling component whenever `select(state)` changes. */
export function useUi<T>(select: (s: UiState) => T): T {
  const [, rerender] = useState(0);
  const pick = useRef(select);
  pick.current = select;
  const last = useRef<T>(select(state));
  useEffect(() => {
    const check = () => {
      const next = pick.current(state);
      if (!Object.is(next, last.current)) { last.current = next; rerender((n) => n + 1); }
    };
    listeners.add(check);
    check();
    return () => { listeners.delete(check); };
  }, []);
  const value = select(state);
  last.current = value;
  return value;
}
