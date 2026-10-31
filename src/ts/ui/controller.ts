// The logic behind the components: loading game files and levels, the layer groups, the saved / shared view, the Display
// options and the calls into the WebAssembly module. It writes to the store (store.ts); the components only render it and
// call the functions exported here.
import {
  FileSet, fsAccess, handles, idbGet, rememberHandle, setFromAssetArchives, setFromEntries, setFromHandles, setFromInput,
  setFromZip, setHandles,
} from '../gamefiles.ts';
import {
  MODE_KEY, STORE_KEY, absorbSecretFromHash, folds, prefs, readLocalView, saveFolds, setLayerPrefs, setOptPref,
  store, storedSecret,
} from '../storage.ts';
import { CAR_IDS, config } from '../config/config.ts';
import type { CarId, GameFileCar } from '../config/config.ts';
import type { Catalog, RmeApi, RmeModule, SavedView, SceneInfo, ViewSnapshot } from '../types.ts';
import { OPTS } from './options.ts';
import { state, update } from './store.ts';
import type { LayerGroup, LayerRow } from './store.ts';
import { call, focusCanvas, moduleReady, sceneInfo, setModule, setOption, wasm } from './wasm.ts';

// ---------------------------------------------------------------- saved state (localStorage + URL)
// URL parameters (shareable): g = game, l = level, c = camera "x,y,z,yaw,pitch", v = visible layers, s / t / p = animation
// speed / time / paused, m = in-game powerup meshes, h = highlight beams.
// localStorage keeps the same and, separately, the camera mode (which is deliberately not part of the URL).
const VIEW_PARAMS = ['g', 'l', 'c', 'v', 's', 't', 'p', 'm', 'h'];
const levelKey = (id: string) => String(id).split('/').pop() as string;

function parseUrlView(search: string): SavedView | null {
  const p = new URLSearchParams(search);
  const game = p.get('g');
  if (!game) return null;
  const st: SavedView = { game, level: p.get('l') || '' };
  const c = (p.get('c') || '').split(',').map(Number);
  if (c.length === 5 && c.every(Number.isFinite)) st.cam = c;
  if (p.has('v')) st.layers = p.get('v') ? (p.get('v') as string).split(',') : [];
  if (p.has('s') && Number.isFinite(Number(p.get('s')))) st.speed = Number(p.get('s'));
  if (p.has('t') && Number.isFinite(Number(p.get('t')))) st.time = Number(p.get('t'));
  if (p.has('p')) st.paused = p.get('p') === '1';
  if (p.has('m')) st.pupMeshes = p.get('m') === '1';
  if (p.has('h')) st.beams = p.get('h') !== '0';
  return st;
}

/** The state to return to after the game files are available (consumed by the first level load). */
let restore: SavedView | null = null;

// The view parameters of the address bar are only an import format: once they are loaded (or dropped because the visitor
// chose something else) they are removed. The Share button builds a new link on demand.
function stripViewFromUrl(): void {
  const p = new URLSearchParams(location.search);
  if (!VIEW_PARAMS.some((k) => p.has(k))) return;
  try { history.replaceState(null, '', location.pathname + location.hash); } catch { /* ignore */ }
}
function dropRestore(): void {
  restore = null;
  stripViewFromUrl();
}

let sceneLoaded = false, lastSaved = '';

// Layers are shown in groups, each with a checkbox that switches the whole group. The first group (pickups and
// powerups) is visible by default, plus the props and movers of Misc, which also takes every layer that is not listed.
const LAYER_GROUPS: { title: string; ids: string[] }[] = [
  { title: 'Pickups & powerups', ids: ['powerup', 'timepowerup', 'notable', 'pedpowerup', 'upgrade', 'credit', 'pickup', 'star'] },
  { title: 'Course & paths', ids: ['start', 'checkpoint', 'raceline', 'startline', 'aipath', 'path', 'zone', 'trigger'] },
  { title: 'Mission', ids: ['missionitem', 'mission', 'ped'] },
  { title: 'Misc', ids: ['props', 'movers', 'drone', 'followerpath', 'animpath'] },  // listed first, in this order; then everything else
];
const DEFAULT_VISIBLE_EXTRA = ['props', 'movers'];  // destructible and movable objects are shown from the start
const defaultLayerVisible = (id: string) => (LAYER_GROUPS[0].ids.includes(id) || DEFAULT_VISIBLE_EXTRA.includes(id) ? 1 : 0);

// ---------------------------------------------------------------- status, busy
function setStatus(text: string, error = false): void { update({ status: { text: text || '', error } }); }
function busy(on: boolean, text?: string): void { update({ busy: { on, text: text ?? state.busy.text } }); }
const paint = () => new Promise<void>((r) => requestAnimationFrame(() => setTimeout(r, 0)));
const errorMessage = (e: unknown) => (e instanceof Error ? e.message : String(e));

// ---------------------------------------------------------------- what the C++ side calls
function installApi(): RmeApi {
  const RME: RmeApi = (window.RME = {
    vfs: null,
    ready: false,
    catalog: null,
    pendingSet: null,
    onReady() {
      RME.ready = true;
      if (RME.pendingSet) {
        const s = RME.pendingSet;
        RME.pendingSet = null;
        void useFiles(s);
      }
    },
    onCameraMode(mode) {
      store.set(MODE_KEY, String(mode));
      update({ camMode: mode });
    },
    onTouchMode() { update({ touch: true }); },
    onGameMode(on) {
      update({ gameMode: !!on, hud: { ...state.hud, over: false, messageVisible: false } });
      (document.activeElement as HTMLElement | null)?.blur();
      focusCanvas();
    },
    configureCar(car, fileJson) {
      return configureCarSettings(car, fileJson);
    },
    onGameDebug(gameDebugJson) {
      update({ gameDebug: JSON.parse(gameDebugJson) });
    },
    onGameHud(speedKmh, score, drift, timeLeft, collected, total, gameState, message, messageAge) {
      const t = Math.max(0, Math.ceil(timeLeft));
      const hud = {
        speed: Math.round(speedKmh),
        score,
        drift,
        pups: `${collected} / ${total}`,
        time: gameState === 3 ? '∞' : `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`,
        urgent: gameState === 0 && t <= 10,
        message,
        messageVisible: !!message && messageAge < 2.2,
        over: gameState !== 0 && gameState !== 3,
        overTitle: state.hud.overTitle,
        overText: state.hud.overText,
      };
      if (gameState === 1 || gameState === 2) {
        hud.overTitle = gameState === 2 ? 'All powerups collected!' : 'Time is up!';
        hud.overText = `Score ${score}  ·  ${collected} / ${total} powerups  ·  drift ${drift}`;
      }
      update({ hud });
    },
    onFatal(msg) { setStatus(msg, true); },
    onHover(text, cat, x, y, swatch) {
      update({ tip: text ? { text, cat: cat || '', x, y, swatch: swatch >= 0 ? '#' + swatch.toString(16).padStart(6, '0') : null } : null });
    },
  });
  return RME;
}
let RME: RmeApi;

// ---------------------------------------------------------------- files, games, levels
async function useFiles(set: FileSet): Promise<void> {
  if (!RME.ready) { RME.pendingSet = set; return; }
  busy(true, `Scanning ${set.count} files…`);
  await paint();
  try {
    RME.vfs = set;
    wasm()._rme_files_changed();
    RME.catalog = JSON.parse(await call('rme_catalog', 'string', [], [], true)) as Catalog;
  } catch (e) {
    console.error(e);
    busy(false);
    setStatus('Could not read those files: ' + errorMessage(e), true);
    return;
  }
  busy(false);
  buildGames();
}

function buildGames(): void {
  const detected = (RME.catalog as Catalog).games.filter((g) => g.detected);
  if (!detected.length) {
    update({ games: [], gamesMessage: 'No supported game found', levelGroups: [], levelId: '', dropOpen: true });
    setStatus('Did not find Re-Volt (levels/) or Carmageddon TDR 2000 (ASSETS/) in the dropped files.', true);
    return;
  }
  const games = detected.map((g) => ({ id: g.id, label: `${g.name} (${g.levels.length} levels)` }));
  const gameId = restore && detected.some((g) => g.id === restore?.game) ? restore.game : games[0].id;
  update({ games, gameId, dropOpen: false });
  setStatus(`${detected.map((g) => g.name).join(' and ')} detected.`);
  buildLevels();
}

function buildLevels(): void {
  const game = (RME.catalog as Catalog).games.find((g) => g.id === state.gameId);
  if (!game || !game.levels.length) { update({ levelGroups: [], levelId: '' }); return; }
  const groups = new Map<string, typeof game.levels>();
  for (const l of game.levels) {
    if (!groups.has(l.group)) groups.set(l.group, []);
    (groups.get(l.group) as typeof game.levels).push(l);
  }
  const useGroups = groups.size > 1 && groups.size < game.levels.length;
  const levelGroups = [...groups].map(([title, levels]) => ({ title: useGroups ? title : null, levels: levels.map((l) => ({ id: l.id, name: l.name })) }));
  const all = levelGroups.flatMap((g) => g.levels);
  let levelId = all[0].id;
  if (restore && restore.game === state.gameId) {
    const want = (restore.level || '').toLowerCase();
    const hit = all.find((l) => levelKey(l.id).toLowerCase() === want);
    if (hit) levelId = hit.id;
  }
  update({ levelGroups, levelId });
  void loadSelected();
}

export function chooseGame(id: string): void {
  dropRestore();
  update({ gameId: id });
  buildLevels();
}
export function chooseLevel(id: string): void {
  dropRestore();
  update({ levelId: id });
  void loadSelected();
}

// The wasm loader suspends while reading files, so only one load may run at a time.
let loadBusy = false, loadAgain = false, autoMinimized = false;
let keepCamera: number[] | null = null;  // the view to return to after reloading the same level
async function loadSelected(): Promise<void> {
  if (loadBusy) { loadAgain = true; return; }
  loadBusy = true;
  try { await loadSelectedNow(); } finally {
    loadBusy = false;
    if (loadAgain) { loadAgain = false; void loadSelected(); }
  }
}

async function loadSelectedNow(): Promise<void> {
  const gameId = state.gameId, levelId = state.levelId;
  if (!gameId || !levelId) return;
  busy(true, 'Loading level…');
  await paint();
  const t0 = performance.now();
  let ok = 0;
  try {
    ok = await call('rme_load', 'number', ['string', 'string'], [gameId, levelId], true);
  } catch (e) {
    console.error(e);
    setStatus('Load failed: ' + errorMessage(e), true);
  }
  busy(false);
  if (!ok) {
    setStatus('Could not load level: ' + call('rme_last_error', 'string', [], []), true);
    return;
  }
  focusCanvas();
  applyRestore(levelId);
  if (keepCamera) {
    call('rme_set_camera', null, ['number', 'number', 'number', 'number', 'number'], keepCamera);
    keepCamera = null;
  }
  if (innerWidth < 760 && !state.dockMin && !autoMinimized) {
    autoMinimized = true;
    toggleDock();
  }
  showSceneInfo(((performance.now() - t0) / 1000).toFixed(1));
}

/** Applies the saved / shared view once, right after the level it belongs to has loaded. */
function applyRestore(levelId: string): void {
  const st = restore;
  dropRestore();
  sceneInfo().categories.forEach((c, i) => {
    const v = c.id in prefs.layers ? prefs.layers[c.id] : defaultLayerVisible(c.id);
    wasm()._rme_set_category_visible(i, v ? 1 : 0);
  });
  if (st && st.level && levelKey(levelId).toLowerCase() === st.level.toLowerCase()) {
    const layers = st.layers;
    if (layers) sceneInfo().categories.forEach((c, i) => wasm()._rme_set_category_visible(i, layers.includes(c.id) ? 1 : 0));
    if (st.cam) call('rme_set_camera', null, ['number', 'number', 'number', 'number', 'number'], st.cam);
    if (typeof st.speed === 'number') setOpt('animSpeed', Math.max(0, Math.min(2, st.speed)), false);
    if (typeof st.paused === 'boolean') setAnimPaused(st.paused);
    if (typeof st.pupMeshes === 'boolean') setOpt('pupMeshes', st.pupMeshes ? 1 : 0, false);
    if (typeof st.beams === 'boolean') setOpt('beams', st.beams ? 1 : 0, false);
    if (typeof st.time === 'number') call('rme_set_anim_time', null, ['number'], [st.time]);
  }
  sceneLoaded = true;
  update({ restoreHint: '' });
  saveState();
}

/** What the menu shows about the loaded level: its title, the capabilities of the game and the layer groups. */
function showSceneInfo(seconds: string): void {
  const info = sceneInfo();
  const members = LAYER_GROUPS.map(() => [] as { c: SceneInfo['categories'][number]; i: number }[]);
  info.categories.forEach((c, i) => {
    if (c.id === 'startline') return;  // switched together with the race line
    let g = LAYER_GROUPS.findIndex((x) => x.ids.includes(c.id));
    if (g < 0) g = LAYER_GROUPS.length - 1;
    members[g].push({ c, i });
  });
  {
    const misc = LAYER_GROUPS.length - 1, order = LAYER_GROUPS[misc].ids;
    const rank = (id: string) => { const k = order.indexOf(id); return k < 0 ? order.length : k; };
    members[misc].sort((a, b) => rank(a.c.id) - rank(b.c.id) || a.i - b.i);
  }
  const groups: LayerGroup[] = [];
  LAYER_GROUPS.forEach((group, gi) => {
    if (!members[gi].length) return;
    const rows: LayerRow[] = members[gi].map(({ c, i }) => {
      const idxs = [i];
      if (c.id === 'raceline') {  // the white AI race-line start marks follow the blue race line
        const sl = info.categories.findIndex((x) => x.id === 'startline');
        if (sl >= 0) { idxs.push(sl); wasm()._rme_set_category_visible(sl, c.visible ? 1 : 0); }
      }
      return { id: c.id, name: c.name, color: c.color, count: c.count, visible: c.visible, idxs };
    });
    groups.push({ title: group.title, rows });
  });
  setStatus('');
  update({
    scene: {
      title: info.title,
      info: `${info.info} · loaded in ${seconds}s`,
      canCull: info.canCull,
      hasCollision: info.hasCollision,
      hasPowerupMeshes: info.hasPowerupMeshes,
      hasMovers: !!info.hasMovers,
      canPlay: !!call('rme_game_can_play', 'number', [], []),
      cars: call('rme_game_cars', 'number', [], []),
      groups,
    },
  });
}

function showLayers(groups: LayerGroup[]): void {
  if (state.scene) update({ scene: { ...state.scene, groups } });
}
function setRows(rows: LayerRow[], on: boolean): LayerRow[] {
  for (const r of rows) for (const k of r.idxs) wasm()._rme_set_category_visible(k, on ? 1 : 0);
  return rows.map((r) => ({ ...r, visible: on }));
}
export function toggleLayer(groupIndex: number, rowIndex: number, on: boolean): void {
  if (!state.scene) return;
  const groups = state.scene.groups.map((g, gi) => (gi !== groupIndex ? g : { ...g, rows: g.rows.map((r, ri) => (ri !== rowIndex ? r : setRows([r], on)[0])) }));
  showLayers(groups);
  saveState();
}
export function toggleLayerGroup(groupIndex: number, on: boolean): void {
  if (!state.scene) return;
  showLayers(state.scene.groups.map((g, gi) => (gi !== groupIndex ? g : { ...g, rows: setRows(g.rows, on) })));
  saveState();
}

// ---------------------------------------------------------------- saving and sharing the view
/** Current view as a plain object (null until a level is shown). */
function snapshot(): ViewSnapshot | null {
  if (!sceneLoaded || !moduleReady()) return null;
  const game = state.gameId, level = levelKey(state.levelId);
  if (!game || !level) return null;
  const info = sceneInfo();
  return {
    game, level,
    cam: call('rme_camera_state', 'string', [], []),
    layers: info.categories.filter((c) => c.visible).map((c) => c.id),
    allLayers: info.categories.map((c) => c.id),
    hasMovers: !!info.hasMovers,
    speed: state.opts.animSpeed,
    paused: state.animPaused,
    pupMeshes: !!state.opts.pupMeshes,
    beams: !!state.opts.beams,
    time: call('rme_anim_time', 'number', [], []),
  };
}

// Autosave to localStorage (the address bar is only updated by the Share button).
const SAVE_INTERVAL_MS = 250;  // localStorage is written at most this often
let lastWrite = -1e9, saveTimer = 0;
function saveState(force = false): void {
  if (force) { writeState(true); return; }
  const wait = lastWrite + SAVE_INTERVAL_MS - performance.now();
  if (wait > 0) {  // too soon: remember to save once more when the interval has passed
    if (!saveTimer) saveTimer = window.setTimeout(() => { saveTimer = 0; writeState(false); }, wait);
    return;
  }
  writeState(false);
}
function writeState(force: boolean): void {
  const st = snapshot();
  if (!st) return;
  const key = [st.game, st.level, st.cam, st.layers.join(','), st.speed, st.paused, st.pupMeshes, st.beams, (st.paused || st.speed === 0) ? st.time.toFixed(2) : ''].join('|');
  if (!force && key === lastSaved) return;
  lastSaved = key;
  lastWrite = performance.now();
  setLayerPrefs(new Set(st.layers), st.allLayers);
  const view: SavedView = {
    game: st.game, level: st.level, cam: st.cam.split(',').map(Number), layers: st.layers, speed: st.speed, paused: st.paused,
    pupMeshes: st.pupMeshes, beams: st.beams, time: st.time,
  };
  store.set(STORE_KEY, JSON.stringify(view));
}

function buildShareUrl(): string | null {
  const st = snapshot();
  if (!st) return null;
  const p = new URLSearchParams(location.search);
  for (const k of VIEW_PARAMS) p.delete(k);
  p.set('g', st.game);
  p.set('l', st.level);
  p.set('c', st.cam);
  p.set('v', st.layers.join(','));
  if (st.hasMovers) {
    p.set('s', String(Number(st.speed.toFixed(2))));
    p.set('t', st.time.toFixed(2));
    if (st.paused) p.set('p', '1');
  }
  p.set('m', st.pupMeshes ? '1' : '0');
  p.set('h', st.beams ? '1' : '0');
  const query = p.toString().replace(/%2C/g, ',').replace(/%7C/g, '|');
  const secret = storedSecret();
  return location.origin + location.pathname + '?' + query + (secret ? '#secret=' + encodeURIComponent(secret) : '');
}

export async function shareView(): Promise<void> {
  const url = buildShareUrl();
  if (!url) return;
  history.replaceState(null, '', url);
  let copied = false;
  try { await navigator.clipboard.writeText(url); copied = true; } catch {
    const tmp = document.createElement('textarea');  // no clipboard API (insecure page): copy through a selection
    tmp.value = url;
    document.body.append(tmp);
    tmp.select();
    try { copied = document.execCommand('copy'); } catch { /* ignore */ }
    tmp.remove();
  }
  update({ share: { url, msg: copied ? 'Link copied to the clipboard and set as the page address.' : 'Link set as the page address – copy it from the box above.' } });
}

// ---------------------------------------------------------------- game and camera
/** Drops the player into the level with the driving minigame (the C++ side takes over the keyboard and gamepad). */
export async function startGame(mode: number): Promise<void> {
  busy(true, 'Loading the car…');
  await paint();
  const saved = store.get(PARAMS_KEY);  // the parameters the player edited earlier (a bad text is just not used)
  if (saved) applyGameParams(saved);
  let ok = 0;
  try {
    ok = await call('rme_game_start', 'number', ['number', 'number'], [mode, carToDrive()], true);
    call('rme_game_show_forces', null, ['number'], [state.debugForces ? 1 : 0]);
    call('rme_game_show_spheres', null, ['number'], [state.debugSpheres ? 1 : 0]);
  } catch (e) {
    console.error(e);
    setStatus('Could not start the game: ' + errorMessage(e), true);
  }
  busy(false);
  if (!ok) setStatus('Could not start the game: ' + call('rme_last_error', 'string', [], []), true);
}
const PARAMS_KEY = 'rme.params.v15';  // the edited minigame configuration (JSON text; v1 was the flat format of the first version)

// The car that was configured last (the engine asks for it when a game starts) with what its game files said: the engine's numbers
// are computed again from that when the player changes the configuration.
let configuredCar: { id: CarId; file: GameFileCar } | null = null;

/** The numbers for the car the engine is about to drive: JSON of {globals, car}, or "ERROR: ..." when they cannot be made. */
function configureCarSettings(car: number, fileJson: string): string {
  try {
    const id = CAR_IDS[car];
    if (!id) return `ERROR: there is no car ${car}`;
    const file = JSON.parse(fileJson) as GameFileCar;
    configuredCar = { id, file };
    return JSON.stringify(config.resolveCar(id, file));
  } catch (e) {
    return 'ERROR: ' + errorMessage(e);
  }
}

/** Sends the engine the numbers of the car it drives, after the configuration changed. '' or the reason it did not work. */
function pushSettings(): string {
  if (!configuredCar) return '';  // no game has been started yet: the engine asks when it does
  try {
    return call('rme_config_apply', 'string', ['string'], [JSON.stringify(config.resolveCar(configuredCar.id, configuredCar.file))]);
  } catch (e) {
    return errorMessage(e);
  }
}

/** The configuration as multi-line JSON, sorted (the defaults with `defaults`). */
export function gameParamsJson(defaults = false): string {
  return config.text(defaults);
}
/** Applies the configuration from JSON text; returns '' on success, else the reason (with the line number). */
export function applyGameParams(text: string): string {
  const error = config.apply(text);
  return error || pushSettings();
}
/** Applies the parameters and, when they are valid, remembers them for the next visit. Returns '' or the reason. */
export function saveGameParams(text: string): string {
  const error = applyGameParams(text);
  if (!error) store.set(PARAMS_KEY, text);
  return error;
}
export function setParamsEditor(open: boolean): void {
  update({ paramsOpen: open });
}

/** The car to drive: the chosen one, or the other when only that one's files are there. */
function carToDrive(): number {
  const cars = state.scene?.cars ?? 0;
  return cars & (1 << state.car) ? state.car : cars & 1 ? 0 : 1;
}
/** Chooses the car (remembered). A car other than the one the loaded level was prepared with needs the level to load again. */
export function chooseCar(car: number): void {
  update({ car });
  store.set('rme.car.v1', String(car));
  if (sceneLoaded && call('rme_game_prepared_car', 'number', [], []) >= 0 && call('rme_game_prepared_car', 'number', [], []) !== carToDrive()) {
    keepCamera = call('rme_camera_state', 'string', [], []).split(',').map(Number);
    void loadSelected();
  }
}
/** Draws or hides the hull's collision spheres (remembered); the ones touching the level are red. */
export function setDebugSpheres(on: boolean): void {
  update({ debugSpheres: on });
  store.set('rme.debugSpheres.v1', on ? '1' : '0');
  call('rme_game_show_spheres', null, ['number'], [on ? 1 : 0]);
}
/** Draws or hides the arrows of the forces the wheels push the car with (remembered). */
export function setDebugForces(on: boolean): void {
  update({ debugForces: on });
  store.set('rme.debugForces.v1', on ? '1' : '0');
  call('rme_game_show_forces', null, ['number'], [on ? 1 : 0]);
}
/** Shows or hides the lateral force panel (remembered). */
export function toggleDebugPanel(): void {
  update({ debugPanel: !state.debugPanel });
  store.set('rme.debugPanel.v1', state.debugPanel ? '1' : '0');
}

/** Controls of the on-screen buttons; the numbers are minigame01::Input::Touch. */
export const TOUCH = { handbrake: 0, reset: 1, camera: 2, restart: 3, exit: 4 } as const;
/** The on-screen stick: x steers (-1 left .. 1 right), y is the throttle (1 gas .. -1 brake / reverse). */
export function gameStick(x: number, y: number): void {
  call('rme_game_stick', null, ['number', 'number'], [x, y]);
}
export function gameTouch(control: number, down: boolean): void {
  call('rme_game_touch', null, ['number', 'number'], [control, down ? 1 : 0]);
}
export function stopGame(): void {
  call('rme_game_stop', null, [], []);
  focusCanvas();
}
export function cameraCommand(command: number): void {
  wasm()._rme_camera_command(command);
  focusCanvas();
}
export function setCameraMode(mode: number): void {
  wasm()._rme_set_camera_mode(mode);
  focusCanvas();
}

// ---------------------------------------------------------------- display options
/** Sends an option to the C++ side. The animation speed goes as the effective rate: 0 while paused. */
function pushOption(key: string): void {
  if (key === 'animSpeed') { setOption('animSpeed', state.animPaused ? 0 : state.opts.animSpeed); return; }
  const def = OPTS.find((o) => o.key === key);
  if (def) setOption(def.name, state.opts[key]);
}
/** Changes a Display option; `persist` remembers it for the next visit (not for values taken from a restored view). */
export function setOpt(key: string, value: number, persist = true): void {
  update({ opts: { ...state.opts, [key]: value } });
  pushOption(key);
  if (persist) setOptPref(key, value);
}
/** A change made with the controls. */
export function changeOpt(key: string, value: number): void {
  const before = state.opts[key];
  setOpt(key, value);
  if (key === 'beams' || key === 'pupMeshes') saveState();  // part of the saved / shared view
  if (key === 'optSmooth' && sceneLoaded && value !== before) {  // the meshes are smoothed while the level loads: load it again, from the same spot
    keepCamera = call('rme_camera_state', 'string', [], []).split(',').map(Number);
    void loadSelected();
  }
}
export function setAnimPaused(paused: boolean): void {
  update({ animPaused: paused });
  pushOption('animSpeed');
}
export function togglePause(): void {
  setAnimPaused(!state.animPaused);
  saveState();
}
export function restartAnimation(): void {
  call('rme_set_anim_time', null, ['number'], [0]);
}

// ---------------------------------------------------------------- menu dock
export function toggleDock(): void {
  update({ dockMin: !state.dockMin, tip: null });
}
export function toggleFold(id: string): void {
  folds[id] = state.folds[id] === 1 ? 0 : 1;
  saveFolds();
  update({ folds: { ...folds } });
}

// ---------------------------------------------------------------- input sources
const folderInput = document.createElement('input');
folderInput.type = 'file';
folderInput.webkitdirectory = true;
folderInput.multiple = true;
const zipInput = document.createElement('input');
zipInput.type = 'file';
zipInput.accept = '.zip,application/zip';

export async function openFolder(): Promise<void> {
  if (!fsAccess) { folderInput.click(); return; }
  try {
    const h = await (window.showDirectoryPicker as NonNullable<Window['showDirectoryPicker']>)({ mode: 'read' });
    busy(true, 'Reading folder…');
    await rememberHandle(h);
    void useFiles(await setFromHandles(handles));
  } catch (err) {
    busy(false);
    if ((err as Error).name !== 'AbortError') setStatus('Could not open the folder: ' + errorMessage(err), true);
  }
}
export function chooseZip(): void { zipInput.click(); }

async function openZip(file: File): Promise<void> {
  busy(true, 'Reading zip directory…');
  try { void useFiles(await setFromZip(file)); } catch (err) {
    console.error(err);
    busy(false);
    setStatus('Could not read the zip file: ' + errorMessage(err), true);
  }
}
folderInput.onchange = () => {
  const files = folderInput.files;
  if (files && files.length === 1 && /\.zip$/i.test(files[0].name)) void openZip(files[0]);
  else if (files && files.length) void useFiles(setFromInput(files));
  folderInput.value = '';
};
zipInput.onchange = () => { if (zipInput.files && zipInput.files.length) void openZip(zipInput.files[0]); zipInput.value = ''; };

function installDropTarget(): void {
  let dragDepth = 0;
  window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; update({ dragging: true }); });
  window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; update({ dragging: false }); } });
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', async (e) => {
    e.preventDefault();
    dragDepth = 0;
    update({ dragging: false });
    const dt = e.dataTransfer;
    if (!dt) return;
    // Handles / entries must be requested synchronously, before the first await.
    const items = [...dt.items];
    const handlePromises = items.map((i) => (typeof i.getAsFileSystemHandle === 'function' ? i.getAsFileSystemHandle().catch(() => null) : Promise.resolve(null)));
    const entries = items.map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null)).filter((x): x is FileSystemEntry => !!x);
    const files = [...dt.files];
    busy(true, 'Reading dropped folder…');
    try {
      const zip = files.length === 1 && /\.zip$/i.test(files[0].name) ? files[0] : null;
      const dirHandles = (await Promise.all(handlePromises)).filter((h): h is FileSystemDirectoryHandle => !!h && h.kind === 'directory');
      let set: FileSet;
      if (zip) set = await setFromZip(zip);
      else if (dirHandles.length) {
        for (const h of dirHandles) await rememberHandle(h);
        set = await setFromHandles(handles);
      } else set = await setFromEntries(entries);
      void useFiles(set);
    } catch (err) {
      console.error(err);
      busy(false);
      setStatus('Could not read the dropped items: ' + errorMessage(err), true);
    }
  });
}

// With a secret the game files come from the encrypted archives, automatically: no drop dialog, straight to the loading
// screen. Dropping or choosing files still works afterwards; a wrong secret brings the drop dialog back with the reason.
async function autoLoadAssets(): Promise<void> {
  update({ dropOpen: false });
  busy(true, 'Reading the asset archives…');
  try {
    void useFiles(await setFromAssetArchives(storedSecret()));
  } catch (err) {
    console.error(err);
    busy(false);
    update({ dropOpen: true });
    setStatus('Could not open the assets: ' + errorMessage(err), true);
  }
}

// Reopen remembered folders after a refresh (needs permission; Chromium keeps it for installed / trusted sites).
function showResume(): void {
  update({ resumeLabel: 'Resume: ' + handles.map((h) => h.name).join(', ') });
}
export async function resumeFolders(): Promise<void> {
  try {
    for (const h of handles) if ((await h.requestPermission({ mode: 'read' })) !== 'granted') throw new Error('permission denied');
    busy(true, 'Reopening your game folder…');
    void useFiles(await setFromHandles(handles));
  } catch (err) {
    busy(false);
    setStatus('Could not reopen the folder: ' + errorMessage(err), true);
  }
}
async function reopenFolders(): Promise<void> {
  const saved = await idbGet('handles');
  if (!Array.isArray(saved) || !saved.length) return;
  setHandles(saved as FileSystemDirectoryHandle[]);
  try {
    let granted = true;
    for (const h of handles) if ((await h.queryPermission({ mode: 'read' })) !== 'granted') granted = false;
    if (!granted) { showResume(); return; }
    busy(true, 'Reopening your game folder…');
    void useFiles(await setFromHandles(handles));
  } catch (err) {
    console.warn('resume failed', err);
    busy(false);
    showResume();
  }
}

// ---------------------------------------------------------------- start
/** Boots the page: saved state, input sources and the WebAssembly module. The components must already be rendered. */
export function start(): void {
  absorbSecretFromHash();
  restore = parseUrlView(location.search) || (readLocalView() as SavedView | null);
  {
    const old = restore as (SavedView & { stalks?: boolean }) | null;  // older versions saved the beams as "stalks"
    if (old && old.beams === undefined && old.stalks !== undefined) old.beams = old.stalks;
  }
  RME = installApi();

  const opts = { ...state.opts };
  for (const o of OPTS) if (o.key in prefs.opts) opts[o.key] = Number(prefs.opts[o.key]);
  update({ opts, touch: matchMedia('(pointer: coarse)').matches, camMode: store.get(MODE_KEY) === '0' ? 0 : 1 });

  setInterval(() => saveState(), SAVE_INTERVAL_MS);
  window.addEventListener('pagehide', () => saveState(true));
  setInterval(() => {
    if (moduleReady() && sceneLoaded) update({ animTime: call('rme_anim_time', 'number', [], []) });
  }, 150);
  installDropTarget();
  const canvas = document.getElementById('canvas') as HTMLCanvasElement;
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  // A press on the canvas takes the keyboard back from any input, select or button that had it (the game reads the keys while the
  // canvas has the focus; the mouse handlers of the canvas stop the browser from moving the focus there by itself).
  canvas.addEventListener('pointerdown', () => {
    const active = document.activeElement as HTMLElement | null;
    if (active && active !== canvas && active !== document.body) active.blur();
    canvas.focus();
  });
  canvas.addEventListener('mouseleave', () => update({ tip: null }));

  const useAssetArchives = !!storedSecret();
  if (useAssetArchives) void autoLoadAssets();
  if (restore && restore.game && !useAssetArchives) {
    const names: Record<string, string> = { revolt: 'Re-Volt', carma: 'Carmageddon TDR 2000' };
    update({ restoreHint: `Saved view: ${names[restore.game] || restore.game}${restore.level ? ' – ' + restore.level : ''}. Load the game files to continue there.` });
  }
  if (fsAccess && !useAssetArchives) void reopenFolders();

  window.Module = {
    canvas,
    print: (t: string) => console.log(t),
    printErr: (t: string) => console.warn(t),
    onRuntimeInitialized() {
      setModule(window.Module as RmeModule);
      for (const o of OPTS) pushOption(o.key);  // the controls' values (saved or default) go to the renderer
      wasm()._rme_set_camera_mode(state.camMode);  // orbital unless the visitor chose flying
    },
  };
  const loader = document.createElement('script');
  loader.src = 'rme.js';
  document.body.append(loader);
}
