// Thin browser glue around the WebAssembly app: file access (drag & drop / folder picker),
// the game + level selectors and the layer legend. All parsing and rendering happens in C++.
(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const SKIP_DIRS = new Set(['fmv', 'music', 'sound', 'wavs', 'miles', '__support', 'gallery', 'times']);
  const DEV = /[?&]dev\b/.test(location.search);

  // Shared secret of the dev server (it only serves ./assets to browsers that send it as a cookie). It can arrive as
  // ?secret=...: keep it in a cookie, then drop it from the address bar. The Share button puts it back into the link.
  const SECRET_COOKIE = 'rme_secret';
  const readSecret = () => {
    const m = document.cookie.split(';').map((s) => s.trim()).find((s) => s.startsWith(SECRET_COOKIE + '='));
    try { return m ? decodeURIComponent(m.slice(SECRET_COOKIE.length + 1)) : ''; } catch { return ''; }
  };
  {
    const p = new URLSearchParams(location.search);
    const s = p.get('secret');
    if (s) {
      document.cookie = `${SECRET_COOKIE}=${encodeURIComponent(s)}; path=/; max-age=31536000; SameSite=Strict`;
      const q = location.search.replace(/([?&])secret=[^&]*&?/, '$1').replace(/[?&]$/, '');
      try { history.replaceState(null, '', location.pathname + q + location.hash); } catch { /* ignore */ }
    }
  }

  // ---------------------------------------------------------------- saved state (localStorage + URL)
  // URL parameters (shareable): g = game, l = level, c = camera "x,y,z,yaw,pitch", v = visible layers.
  // localStorage keeps the same and, separately, the camera mode (which is deliberately not part of the URL).
  const STORE_KEY = 'rme.state.v1', MODE_KEY = 'rme.mode.v1';
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch { /* storage unavailable */ } },
  };
  const levelKey = (id) => String(id).split('/').pop();
  function parseUrlState(search) {
    const p = new URLSearchParams(search);
    if (!p.get('g')) return null;
    const st = { game: p.get('g'), level: p.get('l') || '' };
    const c = (p.get('c') || '').split(',').map(Number);
    if (c.length === 5 && c.every(Number.isFinite)) st.cam = c;
    if (p.has('v')) st.layers = p.get('v') ? p.get('v').split(',') : [];
    if (p.has('s') && Number.isFinite(Number(p.get('s')))) st.speed = Number(p.get('s'));
    if (p.has('t') && Number.isFinite(Number(p.get('t')))) st.time = Number(p.get('t'));
    if (p.has('p')) st.paused = p.get('p') === '1';
    if (p.has('m')) st.pupMeshes = p.get('m') === '1';
    if (p.has('h')) st.stalks = p.get('h') !== '0';
    return st;
  }
  function readLocalState() {
    try { return JSON.parse(store.get(STORE_KEY) || 'null'); } catch { return null; }
  }
  // The state to return to after the game files are available (consumed by the first level load).
  let restore = parseUrlState(location.search) || readLocalState();
  let sceneLoaded = false, lastSaved = '';
  let animSpeed = 1, animPaused = false, pupMeshes = true, stalksOn = false;

  // Preferences that outlive a level: layer visibility (by category id, shared by all levels) and the Display options.
  const PREF_KEY = 'rme.prefs.v1';
  let prefs = { layers: {}, opts: {} };
  try {
    const p = JSON.parse(store.get(PREF_KEY) || 'null');
    if (p && typeof p === 'object') prefs = { layers: p.layers || {}, opts: p.opts || {} };
  } catch { /* keep defaults */ }
  const savePrefs = () => store.set(PREF_KEY, JSON.stringify(prefs));
  const setOptPref = (k, v) => { prefs.opts[k] = v; savePrefs(); };

  // Layers are shown in groups, each with a checkbox that switches the whole group. Only the first group (pickups and
  // powerups) is visible by default; the last one (Misc) also takes every layer that is not listed.
  const LAYER_GROUPS = [
    { title: 'Pickups & powerups', ids: ['powerup', 'timepowerup', 'special', 'pedpowerup', 'upgrade', 'credit', 'pickup', 'star'] },
    { title: 'Course & paths', ids: ['start', 'checkpoint', 'raceline', 'startline', 'aipath', 'path', 'zone', 'trigger'] },
    { title: 'Mission', ids: ['missionitem', 'mission', 'ped'] },
    { title: 'Misc', ids: ['props', 'movers', 'drone', 'followerpath', 'animpath'] },  // listed first, in this order; then everything else
  ];
  const defaultLayerVisible = (id) => (LAYER_GROUPS[0].ids.includes(id) ? 1 : 0);

  // ---------------------------------------------------------------- file sets
  class FileSet {
    constructor() { this.map = new Map(); }
    add(path, size, source) { this.map.set(path, { size, source }); }
    get count() { return this.map.size; }
    list() {
      const out = [];
      for (const [p, e] of this.map) out.push(p + '\t' + e.size);
      return out.join('\n');
    }
    async read(path, off, len) {
      const e = this.map.get(path);
      if (!e) throw new Error('no such file: ' + path);
      if (typeof e.source.read === 'function') return e.source.read(off, len);
      if (typeof e.source === 'string') {  // URL (dev server) – ranged request
        const r = await fetch(e.source, { headers: { Range: `bytes=${off}-${off + len - 1}` } });
        const buf = await r.arrayBuffer();
        return r.status === 206 ? buf : buf.slice(off, off + len);
      }
      return e.source.slice(off, off + len).arrayBuffer();
    }
  }

  const readEntries = (reader) => new Promise((res, rej) => reader.readEntries(res, rej));
  const entryFile = (entry) => new Promise((res, rej) => entry.file(res, rej));

  async function walkEntry(entry, set, prefix) {
    if (entry.isFile) {
      const file = await entryFile(entry);
      set.add(prefix + entry.name, file.size, file);
    } else if (entry.isDirectory) {
      if (SKIP_DIRS.has(entry.name.toLowerCase())) return;
      const reader = entry.createReader();
      const children = [];
      for (;;) {
        const batch = await readEntries(reader);
        if (!batch.length) break;
        children.push(...batch);
      }
      const dirPrefix = prefix + entry.name + '/';
      for (let i = 0; i < children.length; i += 64)
        await Promise.all(children.slice(i, i + 64).map((c) => walkEntry(c, set, dirPrefix)));
    }
  }

  async function setFromEntries(entries) {
    const set = new FileSet();
    for (const e of entries) await walkEntry(e, set, '');
    return set;
  }

  // ---- remembered folders (File System Access API, Chromium): lets a refresh reopen the game without re-dropping it
  const fsAccess = typeof window.showDirectoryPicker === 'function';
  const idbOpen = () => new Promise((res, rej) => {
    const r = indexedDB.open('rme', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
  async function idbGet(key) {
    try {
      const db = await idbOpen();
      return await new Promise((res, rej) => {
        const q = db.transaction('kv').objectStore('kv').get(key);
        q.onsuccess = () => res(q.result);
        q.onerror = () => rej(q.error);
      });
    } catch { return undefined; }
  }
  async function idbSet(key, val) {
    try {
      const db = await idbOpen();
      await new Promise((res, rej) => {
        const tx = db.transaction('kv', 'readwrite');
        tx.objectStore('kv').put(val, key);
        tx.oncomplete = res;
        tx.onerror = () => rej(tx.error);
      });
    } catch (e) { console.warn('could not persist folder handle', e); }
  }
  let handles = [];
  async function rememberHandle(h) {
    for (const x of handles) if (await x.isSameEntry(h).catch(() => false)) return;
    handles.push(h);
    await idbSet('handles', handles);
  }
  async function walkHandle(dir, set, prefix) {
    const children = [];
    for await (const [, h] of dir.entries()) children.push(h);
    for (let i = 0; i < children.length; i += 64) {
      await Promise.all(children.slice(i, i + 64).map(async (h) => {
        if (h.kind === 'file') {
          const f = await h.getFile();
          set.add(prefix + h.name, f.size, f);
        } else if (!SKIP_DIRS.has(h.name.toLowerCase())) {
          await walkHandle(h, set, prefix + h.name + '/');
        }
      }));
    }
  }
  async function setFromHandles(list) {
    const set = new FileSet();
    for (const h of list) await walkHandle(h, set, h.name + '/');
    return set;
  }

  function setFromInput(files) {
    const set = new FileSet();
    for (const f of files) {
      const p = f.webkitRelativePath || f.name;
      const parts = p.split('/');
      if (parts.some((x) => SKIP_DIRS.has(x.toLowerCase()))) continue;
      set.add(p, f.size, f);
    }
    return set;
  }

  // ---- .zip archives (for devices that cannot pick folders, e.g. Android). Stored entries are read by range;
  // deflated entries are inflated once with DecompressionStream and cached.
  async function setFromZip(file) {
    const u16 = (d, o) => d.getUint16(o, true), u32 = (d, o) => d.getUint32(o, true);
    const u64 = (d, o) => Number(d.getBigUint64(o, true));
    const tailLen = Math.min(file.size, 65557 + 22);
    const tailBuf = await file.slice(file.size - tailLen).arrayBuffer();
    const tail = new DataView(tailBuf);
    let eocd = -1;
    for (let i = tailLen - 22; i >= 0; i--) if (u32(tail, i) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error('not a zip file');
    let count = u16(tail, eocd + 10), cdSize = u32(tail, eocd + 12), cdOff = u32(tail, eocd + 16);
    if ((count === 0xffff || cdOff === 0xffffffff) && eocd >= 20 && u32(tail, eocd - 20) === 0x07064b50) {
      const z64Off = u64(tail, eocd - 20 + 8);
      const z = new DataView(await file.slice(z64Off, z64Off + 56).arrayBuffer());
      count = u64(z, 32); cdSize = u64(z, 40); cdOff = u64(z, 48);
    }
    const cd = new DataView(await file.slice(cdOff, cdOff + cdSize).arrayBuffer());
    const bytes = new Uint8Array(cd.buffer);
    const dec = new TextDecoder();
    const set = new FileSet();
    let p = 0;
    while (p + 46 <= cd.byteLength && u32(cd, p) === 0x02014b50) {
      const method = u16(cd, p + 10);
      let csize = u32(cd, p + 20), size = u32(cd, p + 24);
      const nameLen = u16(cd, p + 28), extraLen = u16(cd, p + 30), commentLen = u16(cd, p + 32);
      let local = u32(cd, p + 42);
      const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen)).replace(/\\/g, '/');
      for (let x = p + 46 + nameLen, end = x + extraLen; x + 4 <= end;) {  // zip64 extra field
        const id = u16(cd, x), len = u16(cd, x + 2);
        if (id === 1) {
          let q = x + 4;
          if (size === 0xffffffff) { size = u64(cd, q); q += 8; }
          if (csize === 0xffffffff) { csize = u64(cd, q); q += 8; }
          if (local === 0xffffffff) { local = u64(cd, q); }
        }
        x += 4 + len;
      }
      p += 46 + nameLen + extraLen + commentLen;
      if (name.endsWith('/')) continue;
      if (name.split('/').some((s) => SKIP_DIRS.has(s.toLowerCase()))) continue;
      set.add(name, size, makeZipEntry(file, method, local, csize));
    }
    return set;
  }

  function makeZipEntry(file, method, local, csize) {
    let dataStart = -1, cache = null;
    const start = async () => {
      if (dataStart < 0) {
        const h = new DataView(await file.slice(local, local + 30).arrayBuffer());
        dataStart = local + 30 + h.getUint16(26, true) + h.getUint16(28, true);
      }
      return dataStart;
    };
    return {
      async read(off, len) {
        const s = await start();
        if (method === 0) return file.slice(s + off, s + off + len).arrayBuffer();
        if (method !== 8) throw new Error('unsupported zip compression method ' + method);
        if (!cache) {
          const stream = file.slice(s, s + csize).stream().pipeThrough(new DecompressionStream('deflate-raw'));
          cache = await new Response(stream).arrayBuffer();
        }
        return cache.slice(off, off + len);
      },
    };
  }

  async function setFromDevServer() {
    const r = await fetch('/__manifest.json');
    if (!r.ok) throw new Error('no dev manifest');
    const m = await r.json();
    const set = new FileSet();
    for (const f of m.files) set.add(f.path, f.size, '/assets/' + f.path.split('/').map(encodeURIComponent).join('/'));
    return set;
  }

  // ---------------------------------------------------------------- app state
  const RME = (window.RME = {
    vfs: null,
    ready: false,
    catalog: null,
    onReady() { RME.ready = true; if (RME.pendingSet) { const s = RME.pendingSet; RME.pendingSet = null; useFiles(s); } },
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

  RME.FileSet = FileSet;  // exposed for tests
  RME.useFiles = (set) => useFiles(set);
  RME.setFromZip = (f) => setFromZip(f);

  let M = null;  // the emscripten Module
  const call = (name, ret, types, args, async) => M.ccall(name, ret, types, args, { async: !!async });

  function setStatus(text, err) {
    const el = $('status');
    el.textContent = text || '';
    el.style.color = err ? '#ff8a8a' : '';
  }
  function busy(on, text) {
    $('loading').hidden = !on;
    if (text) $('loadingText').textContent = text;
  }
  const paint = () => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 0)));

  async function useFiles(set) {
    if (!RME.ready) { RME.pendingSet = set; return; }
    busy(true, `Scanning ${set.count} files…`);
    await paint();
    try {
      RME.vfs = set;
      M._rme_files_changed();
      const json = await call('rme_catalog', 'string', [], [], true);
      RME.catalog = JSON.parse(json);
    } catch (e) {
      console.error(e);
      busy(false);
      setStatus('Could not read those files: ' + e.message, true);
      return;
    }
    busy(false);
    buildGameSelect();
  }

  function buildGameSelect() {
    const sel = $('gameSel');
    sel.textContent = '';
    const detected = RME.catalog.games.filter((g) => g.detected);
    if (!detected.length) {
      sel.disabled = true;
      sel.append(new Option('No supported game found', ''));
      $('levelSel').disabled = true;
      setStatus('Did not find Re-Volt (levels/) or Carmageddon TDR 2000 (ASSETS/) in the dropped files.', true);
      $('drop').hidden = false;
      return;
    }
    for (const g of detected) sel.append(new Option(`${g.name} (${g.levels.length} levels)`, g.id));
    if (restore && detected.some((g) => g.id === restore.game)) sel.value = restore.game;
    sel.disabled = false;
    setStatus(`${detected.map((g) => g.name).join(' and ')} detected.`);
    $('drop').hidden = true;
    buildLevelSelect();
    sel.onchange = () => { sel.blur(); restore = null; buildLevelSelect(); };
  }

  function buildLevelSelect() {
    const game = RME.catalog.games.find((g) => g.id === $('gameSel').value);
    const sel = $('levelSel');
    sel.textContent = '';
    if (!game || !game.levels.length) { sel.disabled = true; return; }
    const groups = new Map();
    for (const l of game.levels) {
      if (!groups.has(l.group)) groups.set(l.group, []);
      groups.get(l.group).push(l);
    }
    const useGroups = groups.size > 1 && groups.size < game.levels.length;
    for (const [name, levels] of groups) {
      const parent = useGroups ? Object.assign(document.createElement('optgroup'), { label: name }) : sel;
      for (const l of levels) parent.append(new Option(l.name, l.id));
      if (useGroups) sel.append(parent);
    }
    sel.disabled = false;
    if (restore && restore.game === $('gameSel').value) {
      const want = (restore.level || '').toLowerCase();
      const opt = [...sel.options].find((o) => levelKey(o.value).toLowerCase() === want);
      if (opt) sel.value = opt.value;
    }
    sel.onchange = () => { sel.blur(); restore = null; loadSelected(); };
    loadSelected();
  }

  // The wasm loader suspends while reading files, so only one load may run at a time.
  let loadBusy = false, loadAgain = false, autoMinimized = false;
  async function loadSelected() {
    if (loadBusy) { loadAgain = true; return; }
    loadBusy = true;
    try { await loadSelectedNow(); } finally {
      loadBusy = false;
      if (loadAgain) { loadAgain = false; loadSelected(); }
    }
  }

  async function loadSelectedNow() {
    const gameId = $('gameSel').value, levelId = $('levelSel').value;
    if (!gameId || !levelId) return;
    busy(true, 'Loading level…');
    await paint();
    const t0 = performance.now();
    let ok = 0;
    try {
      ok = await call('rme_load', 'number', ['string', 'string'], [gameId, levelId], true);
    } catch (e) {
      console.error(e);
      setStatus('Load failed: ' + e.message, true);
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

  // Applies the saved / shared view once, right after the level it belongs to has loaded.
  function applyRestore(levelId) {
    const st = restore;
    restore = null;
    {
      const info = JSON.parse(call('rme_scene_info', 'string', [], []));
      info.categories.forEach((c, i) => {
        const v = c.id in prefs.layers ? prefs.layers[c.id] : defaultLayerVisible(c.id);
        if (v !== undefined) M._rme_set_category_visible(i, v ? 1 : 0);
      });
    }
    if (st && st.level && levelKey(levelId).toLowerCase() === st.level.toLowerCase()) {
      if (st.layers) {
        const info = JSON.parse(call('rme_scene_info', 'string', [], []));
        info.categories.forEach((c, i) => M._rme_set_category_visible(i, st.layers.includes(c.id) ? 1 : 0));
      }
      if (st.cam) call('rme_set_camera', null, ['number', 'number', 'number', 'number', 'number'], st.cam);
      if (typeof st.speed === 'number') setAnimSpeed(st.speed);
      if (typeof st.paused === 'boolean') setAnimPaused(st.paused);
      if (typeof st.pupMeshes === 'boolean') setPupMeshes(st.pupMeshes);
      if (typeof st.stalks === 'boolean') setStalks(st.stalks);
      if (typeof st.time === 'number') call('rme_set_anim_time', null, ['number'], [st.time]);
    }
    sceneLoaded = true;
    $('restoreHint').hidden = true;
    saveState();
  }

  // Current view as a plain object (null until a level is shown).
  function snapshot() {
    if (!sceneLoaded || !M) return null;
    const game = $('gameSel').value, level = levelKey($('levelSel').value);
    if (!game || !level) return null;
    const info = JSON.parse(call('rme_scene_info', 'string', [], []));
    return {
      game, level,
      cam: call('rme_camera_state', 'string', [], []),
      layers: info.categories.filter((c) => c.visible).map((c) => c.id),
      allLayers: info.categories.map((c) => c.id),
      hasMovers: !!info.hasMovers,
      speed: animSpeed,
      paused: animPaused,
      pupMeshes,
      stalks: stalksOn,
      time: call('rme_anim_time', 'number', [], []),
    };
  }

  // Autosave to localStorage (the address bar is only updated by the Share button).
  const SAVE_INTERVAL_MS = 250;  // localStorage is written at most this often
  let lastWrite = -1e9, saveTimer = 0;
  function saveState(force) {
    if (force) { writeState(true); return; }
    const wait = lastWrite + SAVE_INTERVAL_MS - performance.now();
    if (wait > 0) {  // too soon: remember to save once more when the interval has passed
      if (!saveTimer) saveTimer = setTimeout(() => { saveTimer = 0; writeState(false); }, wait);
      return;
    }
    writeState(false);
  }
  function writeState(force) {
    const st = snapshot();
    if (!st) return;
    const key = [st.game, st.level, st.cam, st.layers.join(','), st.speed, st.paused, st.pupMeshes, st.stalks, (st.paused || st.speed === 0) ? st.time.toFixed(2) : ''].join('|');
    if (!force && key === lastSaved) return;
    lastSaved = key;
    lastWrite = performance.now();
    const seen = new Set(st.layers);
    for (const id of st.allLayers) prefs.layers[id] = seen.has(id) ? 1 : 0;
    savePrefs();
    store.set(STORE_KEY, JSON.stringify({
      game: st.game, level: st.level, cam: st.cam.split(',').map(Number), layers: st.layers, speed: st.speed, paused: st.paused, pupMeshes: st.pupMeshes, stalks: st.stalks, time: st.time,
    }));
  }

  function buildShareUrl() {
    const st = snapshot();
    if (!st) return null;
    const p = new URLSearchParams(location.search);
    for (const k of ['g', 'l', 'c', 'v', 's', 't', 'p', 'm', 'h', 'secret']) p.delete(k);
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
    p.set('h', st.stalks ? '1' : '0');
    const secret = readSecret();
    if (secret) p.set('secret', secret);
    const query = p.toString().replace(/%2C/g, ',').replace(/%7C/g, '|');
    return location.origin + location.pathname + '?' + query;
  }

  async function shareView() {
    const url = buildShareUrl();
    if (!url) return;
    const input = $('shareUrl');
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
  setInterval(saveState, SAVE_INTERVAL_MS);
  window.addEventListener('pagehide', () => saveState(true));

  function showSceneInfo(seconds) {
    const info = JSON.parse(call('rme_scene_info', 'string', [], []));
    $('secLevel').hidden = false;
    $('levelTitle').textContent = info.title;
    for (const id of ['optSun', 'optPoints']) {
      $(id).disabled = !info.lighting;
      $(id + 'Row').style.opacity = info.lighting ? '' : '.5';
      $(id + 'Row').title = info.lighting ? '' : 'This game does not provide in-game lighting data';
    }
    $('levelInfo').textContent = `${info.info} · loaded in ${seconds}s`;
    setStatus('');
    const legend = $('legend');
    legend.textContent = '';
    const hasMovers = !!info.hasMovers;
    $('optBack').disabled = !info.canCull;
    $('optBackRow').style.opacity = info.canCull ? '' : '.5';
    $('optBackRow').title = info.canCull ? '' : 'This game draws every face double sided';
    $('optPupMesh').disabled = !info.hasPowerupMeshes;
    $('optPupMeshRow').style.opacity = info.hasPowerupMeshes ? '' : '.5';
    $('optPupMeshRow').title = info.hasPowerupMeshes ? '' : 'No in-game powerup meshes for this level';
    $('optAnim').disabled = !hasMovers;
    $('animReset').disabled = !hasMovers;
    $('animPlay').disabled = !hasMovers;
    $('optAnimRow').style.opacity = $('animLine').style.opacity = hasMovers ? '' : '.5';
    const GROUPS = LAYER_GROUPS;
    const members = GROUPS.map(() => []);
    info.categories.forEach((c, i) => {
      if (c.id === 'startline') return;  // switched together with the race line
      let g = GROUPS.findIndex((x) => x.ids.includes(c.id));
      if (g < 0) g = GROUPS.length - 1;
      members[g].push({ c, i });
    });
    {
      const misc = GROUPS.length - 1, order = GROUPS[misc].ids;
      const rank = (id) => { const k = order.indexOf(id); return k < 0 ? order.length : k; };
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
      const boxes = [];
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
        cnt.textContent = c.count;
        row.append(cb, sw, name, cnt);
        legend.append(row);
        boxes.push({ cb, count: c.count, i, idxs });
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
  function applyAnimRate() {
    call('rme_set_option', null, ['string', 'number'], ['animSpeed', animPaused ? 0 : animSpeed]);
    $('animLine').classList.toggle('paused', animPaused);
    $('animPlay').title = animPaused ? 'Play animation' : 'Pause animation';
    $('animPlay').setAttribute('aria-label', animPaused ? 'Play animation' : 'Pause animation');
  }
  function setAnimSpeed(v) {
    animSpeed = Math.max(0, Math.min(2, v));
    $('optAnim').value = String(animSpeed);
    applyAnimRate();
  }
  function setStalks(v) {
    stalksOn = !!v;
    $('optStalks').checked = stalksOn;
    call('rme_set_option', null, ['string', 'number'], ['stalks', stalksOn ? 1 : 0]);
  }
  function setPupMeshes(v) {
    pupMeshes = !!v;
    $('optPupMesh').checked = pupMeshes;
    call('rme_set_option', null, ['string', 'number'], ['powerupMeshes', pupMeshes ? 1 : 0]);
  }
  function setAnimPaused(p) {
    animPaused = !!p;
    applyAnimRate();
  }
  setInterval(() => {
    if (!M || !sceneLoaded) return;
    const t = call('rme_anim_time', 'number', [], []);
    $('animReadout').textContent = `×${animSpeed.toFixed(2)}${(animPaused || animSpeed === 0) ? ' (stopped)' : ''} · t = ${t.toFixed(1)} s`;
  }, 150);
  const opt = (name, v) => call('rme_set_option', null, ['string', 'number'], [name, v]);
  const bindRange = (id, name) => { $(id).oninput = (e) => { opt(name, parseFloat(e.target.value)); setOptPref(id, parseFloat(e.target.value)); }; };
  const bindCheck = (id, name) => { $(id).onchange = (e) => { opt(name, e.target.checked ? 1 : 0); setOptPref(id, e.target.checked ? 1 : 0); e.target.blur(); }; };
  const SIMPLE_OPTS = { optExposure: 'exposure', optShade: 'shade', optMarker: 'markerScale', optLine: 'lineWidth', optSpeed: 'speed',
    optTex: 'textured', optGeo: 'geometry', optSun: 'sun', optWire: 'wireframe', optBack: 'backFaces', optPoints: 'points', optXray: 'xray' };
  function bindControls() {
    // saved Display options
    for (const [id, name] of Object.entries(SIMPLE_OPTS)) {
      if (!(id in prefs.opts)) continue;
      const el = $(id), v = Number(prefs.opts[id]);
      if (el.type === 'checkbox') el.checked = !!v; else el.value = String(v);
      opt(name, v);
    }
    if (typeof prefs.opts.animSpeed === 'number') setAnimSpeed(prefs.opts.animSpeed);
    if ('stalks' in prefs.opts) setStalks(!!prefs.opts.stalks);
    if ('pupMeshes' in prefs.opts) setPupMeshes(!!prefs.opts.pupMeshes);
    bindRange('optExposure', 'exposure'); bindRange('optShade', 'shade'); bindRange('optMarker', 'markerScale');
    bindRange('optLine', 'lineWidth'); bindRange('optSpeed', 'speed');
    bindCheck('optTex', 'textured'); bindCheck('optGeo', 'geometry'); bindCheck('optSun', 'sun'); bindCheck('optWire', 'wireframe'); bindCheck('optBack', 'backFaces'); bindCheck('optPoints', 'points'); bindCheck('optXray', 'xray');
    $('optAnim').oninput = (e) => { setAnimSpeed(parseFloat(e.target.value)); setOptPref('animSpeed', animSpeed); };
    $('animReset').onclick = () => { call('rme_set_anim_time', null, ['number'], [0]); $('animReset').blur(); };
    $('optStalks').onchange = (e) => { setStalks(e.target.checked); setOptPref('stalks', e.target.checked ? 1 : 0); e.target.blur(); saveState(); };
    $('optPupMesh').onchange = (e) => { setPupMeshes(e.target.checked); setOptPref('pupMeshes', e.target.checked ? 1 : 0); e.target.blur(); saveState(); };
    $('animPlay').onclick = () => { setAnimPaused(!animPaused); $('animPlay').blur(); saveState(); };
    $('shareBtn').onclick = shareView;
    $('shareUrl').onfocus = (e) => e.target.select();
    $('camReset').onclick = () => { M._rme_camera_command(0); $('canvas').focus(); };
    $('camStart').onclick = () => { M._rme_camera_command(1); $('canvas').focus(); };
    $('camTop').onclick = () => { M._rme_camera_command(2); $('canvas').focus(); };
    $('modeFly').onclick = () => { M._rme_set_camera_mode(0); $('canvas').focus(); };
    $('modeOrbit').onclick = () => { M._rme_set_camera_mode(1); $('canvas').focus(); };
  }

  // ---------------------------------------------------------------- touch
  let touchUiShown = false;
  function showTouchUi() {
    if (touchUiShown) return;
    touchUiShown = true;
    $('helpTouch').hidden = false;
  }
  if (matchMedia('(pointer: coarse)').matches) showTouchUi();

  document.body.classList.add('menu-open');

  // ---------------------------------------------------------------- menu dock
  // Which sections are collapsed / which details are open is remembered in localStorage.
  const FOLD_KEY = 'rme.folds.v1';
  let folds = {};
  try { folds = JSON.parse(store.get(FOLD_KEY) || '{}') || {}; } catch { folds = {}; }
  const saveFolds = () => store.set(FOLD_KEY, JSON.stringify(folds));
  for (const head of document.querySelectorAll('.sechead')) {
    const sec = head.parentElement;
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
  for (const d of document.querySelectorAll('#dock details')) {
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
  const folderInput = $('folderInput');
  async function openFolder() {
    if (!fsAccess) { folderInput.click(); return; }
    try {
      const h = await window.showDirectoryPicker({ mode: 'read' });
      busy(true, 'Reading folder…');
      await rememberHandle(h);
      useFiles(await setFromHandles(handles));
    } catch (err) {
      busy(false);
      if (err.name !== 'AbortError') setStatus('Could not open the folder: ' + err.message, true);
    }
  }
  $('openBtn').onclick = openFolder;
  $('openBtn2').onclick = openFolder;
  async function openZip(file) {
    busy(true, 'Reading zip directory…');
    try { useFiles(await setFromZip(file)); } catch (err) {
      console.error(err);
      busy(false);
      setStatus('Could not read the zip file: ' + err.message, true);
    }
  }
  folderInput.onchange = () => {
    const files = folderInput.files;
    if (files.length === 1 && /\.zip$/i.test(files[0].name)) openZip(files[0]);
    else if (files.length) useFiles(setFromInput(files));
    folderInput.value = '';
  };
  const zipInput = $('zipInput');
  $('zipBtn').onclick = () => zipInput.click();
  $('zipBtn2').onclick = () => zipInput.click();
  zipInput.onchange = () => { if (zipInput.files.length) openZip(zipInput.files[0]); zipInput.value = ''; };

  let dragDepth = 0;
  window.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; $('dropHint').hidden = false; document.body.classList.add('dragging'); });
  window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('dropHint').hidden = true; document.body.classList.remove('dragging'); } });
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', async (e) => {
    e.preventDefault();
    dragDepth = 0;
    $('dropHint').hidden = true;
    document.body.classList.remove('dragging');
    // Handles / entries must be requested synchronously, before the first await.
    const items = [...e.dataTransfer.items];
    const handlePromises = items.map((i) => (typeof i.getAsFileSystemHandle === 'function' ? i.getAsFileSystemHandle().catch(() => null) : Promise.resolve(null)));
    const entries = items.map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null)).filter(Boolean);
    const files = [...e.dataTransfer.files];
    busy(true, 'Reading dropped folder…');
    try {
      const zip = files.length === 1 && /\.zip$/i.test(files[0].name) ? files[0] : null;
      const dirHandles = (await Promise.all(handlePromises)).filter((h) => h && h.kind === 'directory');
      let set;
      if (zip) set = await setFromZip(zip);
      else if (dirHandles.length) {
        for (const h of dirHandles) await rememberHandle(h);
        set = await setFromHandles(handles);
      } else set = await setFromEntries(entries);
      useFiles(set);
    } catch (err) {
      console.error(err);
      busy(false);
      setStatus('Could not read the dropped items: ' + err.message, true);
    }
  });
  $('canvas').addEventListener('contextmenu', (e) => e.preventDefault());
  $('canvas').addEventListener('mouseleave', () => { $('tip').hidden = true; });

  // Dev server (see serve.mjs) exposes ./assets so the app can be tested without dragging.
  fetch('/__manifest.json', { method: 'HEAD' }).then((r) => {
    if (!r.ok) return;
    $('devBtn').hidden = false;
    $('devBtn').onclick = async () => { busy(true, 'Reading dev manifest…'); useFiles(await setFromDevServer()); };
    if (/[?&]dev\b/.test(location.search)) $('devBtn').click();
  }).catch(() => {});

  // Reopen remembered folders after a refresh (needs permission; Chromium keeps it for installed / trusted sites).
  function showResume() {
    $('resumeBtn').hidden = false;
    $('resumeBtn').textContent = 'Resume: ' + handles.map((h) => h.name).join(', ');
  }
  $('resumeBtn').onclick = async () => {
    try {
      for (const h of handles) if ((await h.requestPermission({ mode: 'read' })) !== 'granted') throw new Error('permission denied');
      busy(true, 'Reopening your game folder…');
      useFiles(await setFromHandles(handles));
    } catch (err) {
      busy(false);
      setStatus('Could not reopen the folder: ' + err.message, true);
    }
  };
  async function resumeFolders() {
    if (!fsAccess || DEV) return;
    const saved = await idbGet('handles');
    if (!Array.isArray(saved) || !saved.length) return;
    handles = saved;
    try {
      let granted = true;
      for (const h of handles) if ((await h.queryPermission({ mode: 'read' })) !== 'granted') granted = false;
      if (!granted) { showResume(); return; }
      busy(true, 'Reopening your game folder…');
      useFiles(await setFromHandles(handles));
    } catch (err) {
      console.warn('resume failed', err);
      busy(false);
      showResume();
    }
  }
  if (restore && restore.game) {
    const names = { revolt: 'Re-Volt', carma: 'Carmageddon TDR 2000' };
    $('restoreHint').textContent = `Saved view: ${names[restore.game] || restore.game}${restore.level ? ' – ' + restore.level : ''}. Load the game files to continue there.`;
    $('restoreHint').hidden = false;
  }
  resumeFolders();

  // ---------------------------------------------------------------- boot the wasm module
  window.Module = {
    canvas: $('canvas'),
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
    onRuntimeInitialized() {
      M = window.Module;
      bindControls();
      M._rme_set_camera_mode(store.get(MODE_KEY) === '0' ? 0 : 1);  // orbital unless the visitor chose flying
    },
  };
  const s = document.createElement('script');
  s.src = 'rme.js';
  document.body.append(s);
})();
