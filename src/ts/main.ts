// Thin browser glue around the WebAssembly app: file access (drag & drop / folder picker / encrypted archives), the game +
// level selectors, the layer legend and the display controls. All parsing and rendering happens in C++.
import { $, $input } from './dom.ts';
import {
  FileSet, fsAccess, handles, idbGet, rememberHandle, setFromAssetArchives, setFromEntries, setFromHandles, setFromInput,
  setFromZip, setHandles,
} from './gamefiles.ts';
import {
  MODE_KEY, STORE_KEY, absorbSecretFromHash, folds, prefs, readLocalView, saveFolds, setLayerPrefs, setOptPref,
  store, storedSecret,
} from './storage.ts';
import type { CType, Catalog, RmeApi, RmeModule, SavedView, SceneInfo, ViewSnapshot } from './types.ts';

absorbSecretFromHash();

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
let restore: SavedView | null = parseUrlView(location.search) || (readLocalView() as SavedView | null);
{
  const old = restore as (SavedView & { stalks?: boolean }) | null;  // older versions saved the beams as "stalks"
  if (old && old.beams === undefined && old.stalks !== undefined) old.beams = old.stalks;
}

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
let animSpeed = 1, animPaused = false, pupMeshes = true, beamsOn = false;

// Layers are shown in groups, each with a checkbox that switches the whole group. Only the first group (pickups and
// powerups) is visible by default; the last one (Misc) also takes every layer that is not listed.
const LAYER_GROUPS: { title: string; ids: string[] }[] = [
  { title: 'Pickups & powerups', ids: ['powerup', 'timepowerup', 'special', 'pedpowerup', 'upgrade', 'credit', 'pickup', 'star'] },
  { title: 'Course & paths', ids: ['start', 'checkpoint', 'raceline', 'startline', 'aipath', 'path', 'zone', 'trigger'] },
  { title: 'Mission', ids: ['missionitem', 'mission', 'ped'] },
  { title: 'Misc', ids: ['props', 'movers', 'drone', 'followerpath', 'animpath'] },  // listed first, in this order; then everything else
];
const defaultLayerVisible = (id: string) => (LAYER_GROUPS[0].ids.includes(id) ? 1 : 0);

// ---------------------------------------------------------------- app state
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
    $('modeFly').classList.toggle('on', mode === 0);
    $('modeOrbit').classList.toggle('on', mode === 1);
    $('modeFly').setAttribute('aria-checked', String(mode === 0));
    $('modeOrbit').setAttribute('aria-checked', String(mode === 1));
    $('helpFly').hidden = mode !== 0;
    $('helpOrbit').hidden = mode !== 1;
  },
  onTouchMode() { showTouchUi(); },
  onGameMode(on) {
    document.body.classList.toggle('playing', !!on);
    $('gameHud').hidden = !on;
    $('ghOver').hidden = true;
    $('ghMsg').style.opacity = '0';
    (document.activeElement as HTMLElement | null)?.blur();
    $('canvas').focus();
  },
  onGameHud(speedKmh, score, drift, timeLeft, collected, total, state, message, messageAge) {
    const t = Math.max(0, Math.ceil(timeLeft));
    $('ghSpeed').textContent = String(Math.round(speedKmh));
    $('ghScore').textContent = String(score);
    $('ghDrift').textContent = String(drift);
    $('ghPups').textContent = `${collected} / ${total}`;
    $('ghTime').textContent = `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
    $('ghTime').style.color = state === 0 && t <= 10 ? '#ff6b6b' : '';
    const msg = $('ghMsg');
    msg.textContent = message;
    msg.style.opacity = message && messageAge < 2.2 ? '1' : '0';
    $('ghOver').hidden = state === 0 || state === 3;
    if (state === 3) $('ghTime').textContent = '∞';
    if (state === 1 || state === 2) {
      $('ghOverTitle').textContent = state === 2 ? 'All powerups collected!' : 'Time is up!';
      $('ghOverText').textContent = `Score ${score}  ·  ${collected} / ${total} powerups  ·  drift ${drift}`;
    }
  },
  onFatal(msg) { setStatus(msg, true); },
  onHover(text, cat, x, y) {
    const tip = $('tip');
    if (!text) { tip.hidden = true; return; }
    tip.textContent = '';
    tip.append(text);
    const small = document.createElement('small');
    small.textContent = cat;
    tip.append(small);
    tip.hidden = false;
    const w = tip.offsetWidth, h = tip.offsetHeight;
    tip.style.left = Math.min(x + 14, innerWidth - w - 8) + 'px';
    tip.style.top = Math.min(y + 14, innerHeight - h - 8) + 'px';
  },
});

let M: RmeModule;  // the emscripten Module, once its runtime is initialised
const call = (name: string, ret: CType, types: string[], args: unknown[], async = false) => M.ccall(name, ret, types, args, { async });
const sceneInfo = (): SceneInfo => JSON.parse(call('rme_scene_info', 'string', [], []));

function setStatus(text: string, err = false): void {
  const el = $('status');
  el.textContent = text || '';
  el.style.color = err ? '#ff8a8a' : '';
}
function busy(on: boolean, text?: string): void {
  $('loading').hidden = !on;
  if (text) $('loadingText').textContent = text;
}
const paint = () => new Promise<void>((r) => requestAnimationFrame(() => setTimeout(r, 0)));
const errorMessage = (e: unknown) => (e instanceof Error ? e.message : String(e));

async function useFiles(set: FileSet): Promise<void> {
  if (!RME.ready) { RME.pendingSet = set; return; }
  busy(true, `Scanning ${set.count} files…`);
  await paint();
  try {
    RME.vfs = set;
    M._rme_files_changed();
    RME.catalog = JSON.parse(await call('rme_catalog', 'string', [], [], true)) as Catalog;
  } catch (e) {
    console.error(e);
    busy(false);
    setStatus('Could not read those files: ' + errorMessage(e), true);
    return;
  }
  busy(false);
  buildGameSelect();
}

function buildGameSelect(): void {
  const sel = $<HTMLSelectElement>('gameSel');
  sel.textContent = '';
  const detected = (RME.catalog as Catalog).games.filter((g) => g.detected);
  if (!detected.length) {
    sel.disabled = true;
    sel.append(new Option('No supported game found', ''));
    $<HTMLSelectElement>('levelSel').disabled = true;
    setStatus('Did not find Re-Volt (levels/) or Carmageddon TDR 2000 (ASSETS/) in the dropped files.', true);
    $('drop').hidden = false;
    return;
  }
  for (const g of detected) sel.append(new Option(`${g.name} (${g.levels.length} levels)`, g.id));
  if (restore && detected.some((g) => g.id === restore?.game)) sel.value = restore.game;
  sel.disabled = false;
  setStatus(`${detected.map((g) => g.name).join(' and ')} detected.`);
  $('drop').hidden = true;
  buildLevelSelect();
  sel.onchange = () => { sel.blur(); dropRestore(); buildLevelSelect(); };
}

function buildLevelSelect(): void {
  const game = (RME.catalog as Catalog).games.find((g) => g.id === $<HTMLSelectElement>('gameSel').value);
  const sel = $<HTMLSelectElement>('levelSel');
  sel.textContent = '';
  if (!game || !game.levels.length) { sel.disabled = true; return; }
  const groups = new Map<string, typeof game.levels>();
  for (const l of game.levels) {
    if (!groups.has(l.group)) groups.set(l.group, []);
    (groups.get(l.group) as typeof game.levels).push(l);
  }
  const useGroups = groups.size > 1 && groups.size < game.levels.length;
  for (const [name, levels] of groups) {
    const parent: HTMLElement = useGroups ? Object.assign(document.createElement('optgroup'), { label: name }) : sel;
    for (const l of levels) parent.append(new Option(l.name, l.id));
    if (useGroups) sel.append(parent);
  }
  sel.disabled = false;
  if (restore && restore.game === $<HTMLSelectElement>('gameSel').value) {
    const want = (restore.level || '').toLowerCase();
    const opt = [...sel.options].find((o) => levelKey(o.value).toLowerCase() === want);
    if (opt) sel.value = opt.value;
  }
  sel.onchange = () => { sel.blur(); dropRestore(); void loadSelected(); };
  void loadSelected();
}

// The wasm loader suspends while reading files, so only one load may run at a time.
let loadBusy = false, loadAgain = false, autoMinimized = false;
async function loadSelected(): Promise<void> {
  if (loadBusy) { loadAgain = true; return; }
  loadBusy = true;
  try { await loadSelectedNow(); } finally {
    loadBusy = false;
    if (loadAgain) { loadAgain = false; void loadSelected(); }
  }
}

async function loadSelectedNow(): Promise<void> {
  const gameId = $<HTMLSelectElement>('gameSel').value, levelId = $<HTMLSelectElement>('levelSel').value;
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
  $('canvas').focus();
  applyRestore(levelId);
  if (innerWidth < 760 && !$('dock').classList.contains('minimized') && !autoMinimized) {
    autoMinimized = true;
    $('dockHide').click();
  }
  showSceneInfo(((performance.now() - t0) / 1000).toFixed(1));
}

/** Applies the saved / shared view once, right after the level it belongs to has loaded. */
function applyRestore(levelId: string): void {
  const st = restore;
  dropRestore();
  sceneInfo().categories.forEach((c, i) => {
    const v = c.id in prefs.layers ? prefs.layers[c.id] : defaultLayerVisible(c.id);
    M._rme_set_category_visible(i, v ? 1 : 0);
  });
  if (st && st.level && levelKey(levelId).toLowerCase() === st.level.toLowerCase()) {
    const layers = st.layers;
    if (layers) sceneInfo().categories.forEach((c, i) => M._rme_set_category_visible(i, layers.includes(c.id) ? 1 : 0));
    if (st.cam) call('rme_set_camera', null, ['number', 'number', 'number', 'number', 'number'], st.cam);
    if (typeof st.speed === 'number') setAnimSpeed(st.speed);
    if (typeof st.paused === 'boolean') setAnimPaused(st.paused);
    if (typeof st.pupMeshes === 'boolean') setPupMeshes(st.pupMeshes);
    if (typeof st.beams === 'boolean') setBeams(st.beams);
    if (typeof st.time === 'number') call('rme_set_anim_time', null, ['number'], [st.time]);
  }
  sceneLoaded = true;
  $('restoreHint').hidden = true;
  saveState();
}

/** Current view as a plain object (null until a level is shown). */
function snapshot(): ViewSnapshot | null {
  if (!sceneLoaded || !M) return null;
  const game = $<HTMLSelectElement>('gameSel').value, level = levelKey($<HTMLSelectElement>('levelSel').value);
  if (!game || !level) return null;
  const info = sceneInfo();
  return {
    game, level,
    cam: call('rme_camera_state', 'string', [], []),
    layers: info.categories.filter((c) => c.visible).map((c) => c.id),
    allLayers: info.categories.map((c) => c.id),
    hasMovers: !!info.hasMovers,
    speed: animSpeed,
    paused: animPaused,
    pupMeshes,
    beams: beamsOn,
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

async function shareView(): Promise<void> {
  const url = buildShareUrl();
  if (!url) return;
  const input = $input('shareUrl');
  input.hidden = false;
  input.value = url;
  history.replaceState(null, '', url);
  let copied = false;
  try { await navigator.clipboard.writeText(url); copied = true; } catch {
    input.focus();
    input.select();
    try { copied = document.execCommand('copy'); } catch { /* ignore */ }
  }
  $('shareMsg').textContent = copied ? 'Link copied to the clipboard and set as the page address.' : 'Link set as the page address – copy it from the box above.';
  input.select();
}
setInterval(() => saveState(), SAVE_INTERVAL_MS);
window.addEventListener('pagehide', () => saveState(true));

/** Drops the player into the level with the driving minigame (the C++ side takes over the keyboard and gamepad). */
async function startGame(mode: number): Promise<void> {
  busy(true, 'Loading the car…');
  await paint();
  let ok = 0;
  try {
    ok = await call('rme_game_start', 'number', ['number'], [mode], true);
  } catch (e) {
    console.error(e);
    setStatus('Could not start the game: ' + errorMessage(e), true);
  }
  busy(false);
  if (!ok) setStatus('Could not start the game: ' + call('rme_last_error', 'string', [], []), true);
}

function showSceneInfo(seconds: string): void {
  const info = sceneInfo();
  $('playRow').hidden = !call('rme_game_can_play', 'number', [], []);
  $('secLevel').hidden = false;
  $('levelTitle').textContent = info.title;
  for (const id of ['optSun', 'optPoints']) {
    $input(id).disabled = !info.lighting;
    $(id + 'Row').style.opacity = info.lighting ? '' : '.5';
    $(id + 'Row').title = info.lighting ? '' : 'This game does not provide in-game lighting data';
  }
  $('levelInfo').textContent = `${info.info} · loaded in ${seconds}s`;
  setStatus('');
  const legend = $('legend');
  legend.textContent = '';
  const hasMovers = !!info.hasMovers;
  $input('optBack').disabled = !info.canCull;
  $('optBackRow').style.opacity = info.canCull ? '' : '.5';
  $('optBackRow').title = info.canCull ? '' : 'This game draws every face double sided';
  $input('optPupMesh').disabled = !info.hasPowerupMeshes;
  $('optPupMeshRow').style.opacity = info.hasPowerupMeshes ? '' : '.5';
  $('optPupMeshRow').title = info.hasPowerupMeshes ? '' : 'No in-game powerup meshes for this level';
  $input('optAnim').disabled = !hasMovers;
  $<HTMLButtonElement>('animReset').disabled = !hasMovers;
  $<HTMLButtonElement>('animPlay').disabled = !hasMovers;
  $('optAnimRow').style.opacity = $('animLine').style.opacity = hasMovers ? '' : '.5';
  const GROUPS = LAYER_GROUPS;
  const members = GROUPS.map(() => [] as { c: SceneInfo['categories'][number]; i: number }[]);
  info.categories.forEach((c, i) => {
    if (c.id === 'startline') return;  // switched together with the race line
    let g = GROUPS.findIndex((x) => x.ids.includes(c.id));
    if (g < 0) g = GROUPS.length - 1;
    members[g].push({ c, i });
  });
  {
    const misc = GROUPS.length - 1, order = GROUPS[misc].ids;
    const rank = (id: string) => { const k = order.indexOf(id); return k < 0 ? order.length : k; };
    members[misc].sort((a, b) => rank(a.c.id) - rank(b.c.id) || a.i - b.i);
  }
  GROUPS.forEach((group, gi) => {
    if (!members[gi].length) return;
    const head = document.createElement('label');
    head.className = 'lgroup';
    const gcb = document.createElement('input');
    gcb.type = 'checkbox';
    const title = document.createElement('span');
    title.textContent = group.title;
    head.append(gcb, title);
    legend.append(head);
    const boxes: { cb: HTMLInputElement; count: number; idxs: number[] }[] = [];
    const refreshGroup = () => {
      const used = boxes.filter((x) => x.count > 0);
      const on = used.filter((x) => x.cb.checked).length;
      gcb.checked = used.length > 0 && on === used.length;
      gcb.indeterminate = on > 0 && on < used.length;
    };
    for (const { c, i } of members[gi]) {
      const row = document.createElement('label');
      row.className = 'item' + (c.count ? '' : ' empty');
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = c.visible;
      const idxs = [i];
      if (c.id === 'raceline') {  // the white AI race-line start marks follow the blue race line
        const sl = info.categories.findIndex((x) => x.id === 'startline');
        if (sl >= 0) { idxs.push(sl); M._rme_set_category_visible(sl, c.visible ? 1 : 0); }
      }
      cb.onchange = () => { for (const k of idxs) M._rme_set_category_visible(k, cb.checked ? 1 : 0); cb.blur(); refreshGroup(); saveState(); };
      const sw = document.createElement('span');
      sw.className = 'sw';
      sw.style.background = c.color;
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = c.name;
      const cnt = document.createElement('span');
      cnt.className = 'count';
      cnt.textContent = String(c.count);
      row.append(cb, sw, name, cnt);
      legend.append(row);
      boxes.push({ cb, count: c.count, idxs });
    }
    gcb.onchange = () => {
      const v = gcb.checked;
      for (const x of boxes) { x.cb.checked = v; for (const k of x.idxs) M._rme_set_category_visible(k, v ? 1 : 0); }
      gcb.indeterminate = false;
      gcb.blur();
      saveState();
    };
    refreshGroup();
  });
}

// ---------------------------------------------------------------- controls
// Effective animation rate = slider speed, or 0 while paused.
function applyAnimRate(): void {
  call('rme_set_option', null, ['string', 'number'], ['animSpeed', animPaused ? 0 : animSpeed]);
  $('animLine').classList.toggle('paused', animPaused);
  $('animPlay').title = animPaused ? 'Play animation' : 'Pause animation';
  $('animPlay').setAttribute('aria-label', animPaused ? 'Play animation' : 'Pause animation');
}
function setAnimSpeed(v: number): void {
  animSpeed = Math.max(0, Math.min(2, v));
  $input('optAnim').value = String(animSpeed);
  applyAnimRate();
}
function setBeams(v: boolean): void {
  beamsOn = !!v;
  $input('optBeams').checked = beamsOn;
  call('rme_set_option', null, ['string', 'number'], ['beams', beamsOn ? 1 : 0]);
}
function setPupMeshes(v: boolean): void {
  pupMeshes = !!v;
  $input('optPupMesh').checked = pupMeshes;
  call('rme_set_option', null, ['string', 'number'], ['powerupMeshes', pupMeshes ? 1 : 0]);
}
function setAnimPaused(p: boolean): void {
  animPaused = !!p;
  applyAnimRate();
}
setInterval(() => {
  if (!M || !sceneLoaded) return;
  const t: number = call('rme_anim_time', 'number', [], []);
  $('animReadout').textContent = `×${animSpeed.toFixed(2)}${(animPaused || animSpeed === 0) ? ' (stopped)' : ''} · t = ${t.toFixed(1)} s`;
}, 150);

const opt = (name: string, v: number) => call('rme_set_option', null, ['string', 'number'], [name, v]);
const bindRange = (id: string, name: string) => {
  const el = $input(id);
  el.oninput = () => { const v = parseFloat(el.value); opt(name, v); setOptPref(id, v); };
};
const bindCheck = (id: string, name: string) => {
  const el = $input(id);
  el.onchange = () => { const v = el.checked ? 1 : 0; opt(name, v); setOptPref(id, v); el.blur(); };
};
const SIMPLE_OPTS: Record<string, string> = {
  optExposure: 'exposure', optPupExposure: 'powerupExposure', optShade: 'shade', optMarker: 'markerScale', optLine: 'lineWidth', optSpeed: 'speed',
  optTex: 'textured', optGeo: 'geometry', optSun: 'sun', optWire: 'wireframe', optBack: 'backFaces', optPoints: 'points', optXray: 'xray',
};

function bindControls(): void {
  // saved Display options
  for (const [id, name] of Object.entries(SIMPLE_OPTS)) {
    if (!(id in prefs.opts)) continue;
    const el = $input(id), v = Number(prefs.opts[id]);
    if (el.type === 'checkbox') el.checked = !!v; else el.value = String(v);
    opt(name, v);
  }
  if (typeof prefs.opts.animSpeed === 'number') setAnimSpeed(prefs.opts.animSpeed);
  if ('beams' in prefs.opts) setBeams(!!prefs.opts.beams);
  if ('pupMeshes' in prefs.opts) setPupMeshes(!!prefs.opts.pupMeshes);
  bindRange('optExposure', 'exposure'); bindRange('optPupExposure', 'powerupExposure'); bindRange('optShade', 'shade'); bindRange('optMarker', 'markerScale');
  bindRange('optLine', 'lineWidth'); bindRange('optSpeed', 'speed');
  bindCheck('optTex', 'textured'); bindCheck('optGeo', 'geometry'); bindCheck('optSun', 'sun'); bindCheck('optWire', 'wireframe');
  bindCheck('optBack', 'backFaces'); bindCheck('optPoints', 'points'); bindCheck('optXray', 'xray');
  $input('optAnim').oninput = () => { setAnimSpeed(parseFloat($input('optAnim').value)); setOptPref('animSpeed', animSpeed); };
  $('animReset').onclick = () => { call('rme_set_anim_time', null, ['number'], [0]); $('animReset').blur(); };
  $input('optBeams').onchange = () => { const on = $input('optBeams').checked; setBeams(on); setOptPref('beams', on ? 1 : 0); $('optBeams').blur(); saveState(); };
  $input('optPupMesh').onchange = () => { const on = $input('optPupMesh').checked; setPupMeshes(on); setOptPref('pupMeshes', on ? 1 : 0); $('optPupMesh').blur(); saveState(); };
  $('animPlay').onclick = () => { setAnimPaused(!animPaused); $('animPlay').blur(); saveState(); };
  $('shareBtn').onclick = () => { void shareView(); };
  $input('shareUrl').onfocus = () => $input('shareUrl').select();
  $('camReset').onclick = () => { M._rme_camera_command(0); $('canvas').focus(); };
  $('camStart').onclick = () => { M._rme_camera_command(1); $('canvas').focus(); };
  $('camTop').onclick = () => { M._rme_camera_command(2); $('canvas').focus(); };
  $('modeFly').onclick = () => { M._rme_set_camera_mode(0); $('canvas').focus(); };
  $('modeOrbit').onclick = () => { M._rme_set_camera_mode(1); $('canvas').focus(); };
  $('playBtn').onclick = () => { void startGame(0); };
  $('freeBtn').onclick = () => { void startGame(1); };
  $('gameBack').onclick = () => { call('rme_game_stop', null, [], []); $('canvas').focus(); };
}

// ---------------------------------------------------------------- touch
let touchUiShown = false;
function showTouchUi(): void {
  if (touchUiShown) return;
  touchUiShown = true;
  $('helpTouch').hidden = false;
}
if (matchMedia('(pointer: coarse)').matches) showTouchUi();

document.body.classList.add('menu-open');

// ---------------------------------------------------------------- menu dock
// Which sections are collapsed / which details are open is remembered in localStorage.
for (const head of document.querySelectorAll<HTMLElement>('.sechead')) {
  const sec = head.parentElement as HTMLElement;
  if (folds[sec.id] === 1) {
    sec.classList.add('collapsed');
    head.setAttribute('aria-expanded', 'false');
  }
  head.onclick = () => {
    const collapsed = sec.classList.toggle('collapsed');
    head.setAttribute('aria-expanded', String(!collapsed));
    folds[sec.id] = collapsed ? 1 : 0;
    saveFolds();
  };
}
for (const d of document.querySelectorAll<HTMLDetailsElement>('#dock details')) {
  if (d.id in folds) d.open = folds[d.id] === 1;
  d.addEventListener('toggle', () => { folds[d.id] = d.open ? 1 : 0; saveFolds(); });
}
$('dockHide').onclick = () => {
  const min = $('dock').classList.toggle('minimized');
  $('dockHide').textContent = min ? '☰ Menu' : 'Hide ▴';
  $('dockHide').title = min ? 'Show menu' : 'Hide menu';
  $('dockHide').setAttribute('aria-expanded', String(!min));
  document.body.classList.toggle('menu-open', !min);
  $('tip').hidden = true;
  $('dockHide').blur();
};

// ---------------------------------------------------------------- input sources
const folderInput = $input('folderInput');
async function openFolder(): Promise<void> {
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
$('openBtn').onclick = () => { void openFolder(); };
$('openBtn2').onclick = () => { void openFolder(); };
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
const zipInput = $input('zipInput');
$('zipBtn').onclick = () => zipInput.click();
$('zipBtn2').onclick = () => zipInput.click();
zipInput.onchange = () => { if (zipInput.files && zipInput.files.length) void openZip(zipInput.files[0]); zipInput.value = ''; };

let dragDepth = 0;
window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('dropHint').hidden = false; document.body.classList.add('dragging'); });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('dropHint').hidden = true; document.body.classList.remove('dragging'); } });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', async (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('dropHint').hidden = true;
  document.body.classList.remove('dragging');
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
$('canvas').addEventListener('contextmenu', (e) => e.preventDefault());
$('canvas').addEventListener('mouseleave', () => { $('tip').hidden = true; });

// With a secret the game files come from the encrypted archives, automatically: no drop dialog, straight to the loading
// screen. Dropping or choosing files still works afterwards; a wrong secret brings the drop dialog back with the reason.
async function autoLoadAssets(): Promise<void> {
  $('drop').hidden = true;
  busy(true, 'Reading the asset archives…');
  try {
    void useFiles(await setFromAssetArchives(storedSecret()));
  } catch (err) {
    console.error(err);
    busy(false);
    $('drop').hidden = false;
    setStatus('Could not open the assets: ' + errorMessage(err), true);
  }
}
const useAssetArchives = !!storedSecret();
if (useAssetArchives) void autoLoadAssets();

// Reopen remembered folders after a refresh (needs permission; Chromium keeps it for installed / trusted sites).
function showResume(): void {
  $('resumeBtn').hidden = false;
  $('resumeBtn').textContent = 'Resume: ' + handles.map((h) => h.name).join(', ');
}
$('resumeBtn').onclick = async () => {
  try {
    for (const h of handles) if ((await h.requestPermission({ mode: 'read' })) !== 'granted') throw new Error('permission denied');
    busy(true, 'Reopening your game folder…');
    void useFiles(await setFromHandles(handles));
  } catch (err) {
    busy(false);
    setStatus('Could not reopen the folder: ' + errorMessage(err), true);
  }
};
async function resumeFolders(): Promise<void> {
  if (!fsAccess || useAssetArchives) return;
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
if (restore && restore.game && !useAssetArchives) {
  const names: Record<string, string> = { revolt: 'Re-Volt', carma: 'Carmageddon TDR 2000' };
  $('restoreHint').textContent = `Saved view: ${names[restore.game] || restore.game}${restore.level ? ' – ' + restore.level : ''}. Load the game files to continue there.`;
  $('restoreHint').hidden = false;
}
void resumeFolders();

// ---------------------------------------------------------------- boot the wasm module
window.Module = {
  canvas: $<HTMLCanvasElement>('canvas'),
  print: (t: string) => console.log(t),
  printErr: (t: string) => console.warn(t),
  onRuntimeInitialized() {
    M = window.Module as RmeModule;
    bindControls();
    M._rme_set_camera_mode(store.get(MODE_KEY) === '0' ? 0 : 1);  // orbital unless the visitor chose flying
  },
};
const loader = document.createElement('script');
loader.src = 'rme.js';
document.body.append(loader);
