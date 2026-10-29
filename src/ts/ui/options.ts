// The Display options. `key` is the preference key (kept from the older versions, so saved settings still apply), `name` the
// option of the C++ side (rme_set_option) and `def` the value of the control when nothing was saved.
export interface OptDef { key: string; name: string; def: number }

export const OPTS: OptDef[] = [
  { key: 'optExposure', name: 'exposure', def: 1.35 },
  { key: 'optPupExposure', name: 'haloExposure', def: 1.35 },
  { key: 'optPupScale', name: 'powerupScale', def: 0.1 },
  { key: 'optXray', name: 'xrayStrength', def: 0.2 },
  { key: 'beams', name: 'beams', def: 0 },
  { key: 'pupMeshes', name: 'powerupMeshes', def: 1 },
  { key: 'optLine', name: 'lineWidth', def: 3 },
  { key: 'optSpeed', name: 'speed', def: 1 },
  { key: 'animSpeed', name: 'animSpeed', def: 1 },  // the C++ side gets the effective rate: 0 while paused
  { key: 'optTex', name: 'textured', def: 1 },
  { key: 'optGeo', name: 'geometry', def: 1 },
  { key: 'optCol', name: 'collision', def: 0 },
  { key: 'optSmooth', name: 'smooth', def: 0 },  // read when a level loads: changing it reloads the level
  { key: 'optWire', name: 'wireframe', def: 0 },
  { key: 'optBack', name: 'backFaces', def: 0 },
];
