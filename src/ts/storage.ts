// localStorage: the saved view, the preferences, the fold state of the menu and the secret of the asset archives.

export const store = {
  get(key: string): string | null {
    try { return localStorage.getItem(key); } catch { return null; }
  },
  set(key: string, value: string): void {
    try { localStorage.setItem(key, value); } catch { /* storage unavailable */ }
  },
};

export const STORE_KEY = 'rme.state.v1';  // the saved view
export const MODE_KEY = 'rme.mode.v1';    // camera mode (deliberately not part of shared links)
const PREF_KEY = 'rme.prefs.v1';
const FOLD_KEY = 'rme.folds.v1';
const SECRET_KEY = 'rme.secret.v1';

/** Parses `a=b&c=d` (a query string or a hash) without turning '+' into a space. */
export function parseParams(text: string): Map<string, string> {
  const m = new Map<string, string>();
  for (const part of text.split('&')) {
    if (!part) continue;
    const i = part.indexOf('=');
    try {
      m.set(decodeURIComponent(i < 0 ? part : part.slice(0, i)), i < 0 ? '' : decodeURIComponent(part.slice(i + 1)));
    } catch { /* malformed escape */ }
  }
  return m;
}

/**
 * The secret that opens the encrypted asset archives lives in localStorage. It arrives as a parameter of the address bar's
 * hash (#secret=...), which is parsed like a query string and removed again once it has been stored. The Share button puts
 * it back into the links it makes.
 */
export function absorbSecretFromHash(): void {
  const h = parseParams(location.hash.replace(/^#/, ''));
  const secret = (h.get('secret') || '').trim();
  if (!secret) return;
  store.set(SECRET_KEY, secret);
  h.delete('secret');
  const rest = [...h].map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
  try { history.replaceState(null, '', location.pathname + location.search + (rest ? '#' + rest : '')); } catch { /* ignore */ }
}

export const storedSecret = (): string => store.get(SECRET_KEY) || '';

function readJson(key: string): unknown {
  try { return JSON.parse(store.get(key) || 'null'); } catch { return null; }
}

/** Preferences that outlive a level: layer visibility (by layer id, shared by all levels) and the Display options. */
export interface Prefs {
  layers: Record<string, number>;
  opts: Record<string, number>;
}

export const prefs: Prefs = { layers: {}, opts: {} };
{
  const p = readJson(PREF_KEY) as Partial<Prefs> | null;
  if (p && typeof p === 'object') {
    prefs.layers = p.layers || {};
    prefs.opts = p.opts || {};
  }
}
const savePrefs = () => store.set(PREF_KEY, JSON.stringify(prefs));
export const setOptPref = (key: string, value: number): void => { prefs.opts[key] = value; savePrefs(); };
export const setLayerPrefs = (visible: Set<string>, all: string[]): void => {
  for (const id of all) prefs.layers[id] = visible.has(id) ? 1 : 0;
  savePrefs();
};

/** Which sections of the menu are collapsed / which details are open (1 = collapsed or open, depending on the element). */
export const folds: Record<string, number> = (readJson(FOLD_KEY) as Record<string, number> | null) || {};
export const saveFolds = (): void => store.set(FOLD_KEY, JSON.stringify(folds));

export const readLocalView = (): unknown => readJson(STORE_KEY);
