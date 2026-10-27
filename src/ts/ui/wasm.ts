// The calls into the WebAssembly module (Emscripten): every access goes through here.
import type { CType, RmeModule, SceneInfo } from '../types.ts';

let mod: RmeModule | null = null;

export const setModule = (m: RmeModule): void => { mod = m; };
export const moduleReady = (): boolean => mod !== null;
/** The module's raw exports (only after it is initialised). */
export const wasm = (): RmeModule => mod as RmeModule;

export const call = (name: string, ret: CType, types: string[], args: unknown[], async = false) =>
  (mod as RmeModule).ccall(name, ret, types, args, { async });
export const sceneInfo = (): SceneInfo => JSON.parse(call('rme_scene_info', 'string', [], []));
export const setOption = (name: string, value: number): void => {
  if (mod) call('rme_set_option', null, ['string', 'number'], [name, value]);
};
export const focusCanvas = (): void => document.getElementById('canvas')?.focus();
