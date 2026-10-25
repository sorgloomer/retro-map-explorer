// Shapes shared between the modules and with the WebAssembly side (JSON returned by the C++ exports, the JS API it calls).
import type { FileSet } from './gamefiles.ts';

/** A layer of the level (markers / lines of one kind), as reported by `rme_scene_info`. */
export interface LayerInfo {
  id: string;
  name: string;
  color: string;
  count: number;
  visible: boolean;
}

/** `rme_scene_info`: what the loaded level offers. */
export interface SceneInfo {
  title: string;
  info: string;
  lighting: boolean;
  canCull: boolean;
  hasPowerupMeshes: boolean;
  hasMovers: boolean;
  categories: LayerInfo[];
}

export interface CatalogLevel { id: string; name: string; group: string }
export interface CatalogGame { id: string; name: string; detected: boolean; levels: CatalogLevel[] }
/** `rme_catalog`: the games found in the file set. */
export interface Catalog { games: CatalogGame[] }

/** The view that is saved to localStorage and shared in links (camera = x, y, z, yaw, pitch). */
export interface SavedView {
  game: string;
  level: string;
  cam?: number[];
  layers?: string[];
  speed?: number;
  time?: number;
  paused?: boolean;
  pupMeshes?: boolean;
  stalks?: boolean;
}

/** The current view as collected from the app. */
export interface ViewSnapshot {
  game: string;
  level: string;
  cam: string;
  layers: string[];
  allLayers: string[];
  hasMovers: boolean;
  speed: number;
  paused: boolean;
  pupMeshes: boolean;
  stalks: boolean;
  time: number;
}

export type CType = 'number' | 'string' | null;

/** The Emscripten module (rme.js / rme.wasm): the exports used from JavaScript. */
export interface RmeModule {
  canvas: HTMLCanvasElement;
  print(text: string): void;
  printErr(text: string): void;
  onRuntimeInitialized(): void;
  ccall(name: string, returnType: CType, argTypes: string[], args: unknown[], opts?: { async?: boolean }): any;
  _rme_files_changed(): void;
  _rme_set_category_visible(index: number, visible: number): void;
  _rme_camera_command(command: number): void;
  _rme_set_camera_mode(mode: number): void;
}

/** `window.RME`: what the C++ side calls (see app.cpp and vfs.cpp). */
export interface RmeApi {
  vfs: FileSet | null;
  ready: boolean;
  catalog: Catalog | null;
  pendingSet: FileSet | null;
  onReady(): void;
  onCameraMode(mode: number): void;
  onTouchMode(): void;
  onFatal(message: string): void;
  onHover(text: string | null, category: string | null, x: number, y: number): void;
}
