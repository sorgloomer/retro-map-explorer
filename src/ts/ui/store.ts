// The state the components render: one plain object, replaced piece by piece through update(). Components subscribe to the
// parts they use with useUi(selector) and re-render when the selected value changes (compared with Object.is, so nested
// values are replaced, never mutated).
import { useEffect, useRef, useState } from 'preact/hooks';
import { folds } from '../storage.ts';
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
  hasPowerupMeshes: boolean;
  hasMovers: boolean;
  canPlay: boolean;
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
  tip: { text: string; cat: string; x: number; y: number } | null;
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
