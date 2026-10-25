# Retro Map Explorer

Explore the maps of retro games in 3D from a bird's-eye view, right in the browser.
Written in C++ compiled to WebAssembly, rendered with WebGL2. No engine or framework; the
JavaScript layer only handles file access (drag & drop) and the HTML selectors.

Supported games:

| Game | Folder to drop | What is shown |
|------|----------------|---------------|
| Re-Volt | the one containing `levels/` | world + instanced models, start position, pickups, race line, lights, track zones, triggers, force fields, mirrors, other objects |
| Carmageddon TDR 2000 | the one containing `ASSETS/` | static track meshes with textures, start position, powerups, checkpoints, race line, start/finish line, opponent roam paths, drone paths, pedestrians |

Your game files never leave the browser: files are read lazily from the dropped folder, nothing is uploaded
and nothing is bundled with the app.

## Controls

Two camera modes (switch in the menu or with `V`):

* **Flying** – drag to look, `W A S D` fly along the view direction (so you can change height), `E`/`Space` up, `Q`/`C` down, wheel to dolly.
* **Orbital** – left-drag slides the map horizontally so the point you grabbed stays under the cursor (height never changes);
  right-drag orbits around the point you pressed on; `W A S D` / `Q E` move; wheel zooms towards the pivot.

`Shift` fast, `Alt` slow, `R` overview, `T` start position. Hover a marker for its label.

**Touch screens** (Android, Windows touch) use Google Earth style gestures in both camera modes: one finger drags the map,
pinch zooms towards the touched point, two fingers dragging up/down tilt the view, and twisting two fingers rotates around the
touched midpoint (vertical axis). Tap a marker for its label.

Loading game files: drop the game folder on the page, or use **Open game folder** / **Open .zip**. Browsers on Android cannot pick
folders, so zip the game folder (store without compression for the fastest loading) and open the zip instead.

## Saved view and sharing

The current view (game, level, camera position and angles, visible layers, moving-objects switch, animation speed and the
animation time) is saved to `localStorage` automatically, together with the flying/orbital camera mode, so a refresh
continues where you left off. The address bar is **not** updated continuously: press **Share this view** to build a link
(`?g=<game>&l=<level>&c=<x,y,z,yaw,pitch>&v=<layers>&s=<speed>&t=<time>[&p=1 when paused][&m=1 for in-game powerup meshes]`), which is shown in a box, set as the page address and copied
to the clipboard. A friend who opens it and loads their own game files lands on the same level, camera, layers and, if you froze
the animation, the same animation frame. In Chromium browsers the game folder itself is remembered too (File System Access API),
so a refresh can reopen it; the browser may ask for permission again.

## Build

Requires Node.js (with `npm install` for the page's build tools: TypeScript and rolldown) and the Emscripten SDK (installed locally, nothing global):

```sh
git clone --depth 1 https://github.com/emscripten-core/emsdk .tools/emsdk
.tools/emsdk/emsdk install latest
.tools/emsdk/emsdk activate latest
npm install
node scripts/build.mjs          # -> dist/   (--debug: unoptimised wasm, unminified bundle with a source map; --web-only: skip the wasm)
node scripts/serve.mjs 8080     # http://localhost:8080
```

The page is written in TypeScript (`src/ts/*.ts`). The build type-checks it with `tsc` (`npm run typecheck` does only that), then
rolldown compiles, bundles and minifies it into the single script `dist/app.js`. `dist/` only receives `app.js`, `index.html`,
`style.css` and the wasm files: the TypeScript sources are never published.

`scripts/serve.mjs` serves `dist/` and `./assets` as plain static files (no Range, no ETag, no authentication). The test games
live in `./assets` as AES-encrypted zip archives (`ctdr2k.zip`, `rvlt.zip`) that are split into fixed-size parts for hosts
without Range or ETag support: run `python scripts/chunk_assets.py` to (re)create `ctdr2k.zip.parts/` and `rvlt.zip.parts/`
(`meta.json` with the etag, chunk size, total size and part count; `parts.json` with the name and SHA-256 of every part; and the part files themselves, named after the archive (`ctdr2k000.bin`, `ctdr2k001.bin`, ...), 4 MiB each).
Open the app once as `http://localhost:8080/#secret=<password>`: the secret is moved into localStorage (and removed from the
address bar). With a stored secret the app skips the drop dialog and loads the game files from the encrypted archives right away.
`src/ts/zip/` downloads only the parts it needs (checking each against its SHA-256), decrypts in the browser and caches
the parts in IndexedDB; a cached part is reused while its hash matches `parts.json`, so after an archive changed only the
parts that differ are downloaded again. Share links include `#secret=...` when a secret is stored, so only share them with
people who may see your assets.
`dist/` contains only the app and can be hosted on any static server.

## Layout

```
src/cpp/vmath.h        vector / matrix math
src/cpp/inflate.*      DEFLATE/zlib decoder (no zlib dependency)
src/cpp/image.*        BMP + TGA decoders
src/cpp/vfs.*          file system over the dropped files (JS: list + ranged read, via Asyncify)
src/cpp/scene.h        game independent scene: triangle batches, textures, markers, lines
src/cpp/revolt.cpp     Re-Volt loader (.w .prm .fin .inf .fob .pan .taz .tri .lit .fld .rim)
src/cpp/carma.cpp      Carmageddon TDR 2000 loader (.dir/.pak archives, .hie, .mshs, .tga, .lins, .pup, .scol)
src/cpp/renderer.*     WebGL2 renderer (textured geometry, instanced markers, screen-space line ribbons)
src/cpp/raycast.*      view rays and picking against the level geometry
src/cpp/pan.*          the surface the camera slides on while panning
src/cpp/app.cpp        camera, input, main loop, JS-facing API
src/web/index.html     the page (copied to dist/)
src/web/style.css      its styles (copied to dist/)
src/ts/main.ts         entry point: UI, state, controls, wasm boot (bundled into dist/app.js)
src/ts/gamefiles.ts    where the game files come from: folders, zip files, remembered handles, the encrypted asset archives
src/ts/zip/            one zip reader for a file on the device and for the encrypted remote archives:
  index.ts             openZip(ByteReader): the zip directory and entries (stored by range, deflated as a whole, WinZip AES)
  types.ts             ByteReader (where bytes come from) and ArchiveEntry (what comes out)
  blobreader.ts        ByteReader over a File on the device
  partsreader.ts       ByteReader over a remote folder of parts (download, SHA-256 check, memory cache)
  cache.ts             the persistent IndexedDB cache of the parts
  zipaes.ts            WinZip AES: cipher, key derivation, authentication
src/ts/storage.ts      localStorage: saved view, preferences, secret
src/ts/utils/arrays.ts  array helpers (pushAll)
src/ts/types.ts, dom.ts, globals.d.ts   shared types, typed element lookup, browser API declarations
scripts/               build.mjs, serve.mjs, chunk_assets.py
```

## Adding a game

Implement `Game` (see `src/cpp/game.h`): `detect()`, `listLevels()` and `load()` which fills a `Scene`
(add triangles with `addTriangle` / `addIndexed`, annotations with `addCategory` + `addMarker` / `addLine` / `addBox`)
and register it in `initGames()` in `src/cpp/app.cpp`.

## Known gaps

* Carmageddon: animated water meshes use a different mesh layout and are skipped; dynamic props
  (moveables, trams, trains) are not placed; collision-only volumes are not shown.
* Re-Volt: AI nodes (`.fan`), camera nodes (`.cam`) and visiboxes (`.vis`) are not shown; objects other
  than pickups are only labelled with their numeric type.
* Format notes come from community documentation and reverse engineering; a few details (marker
  orientation, trigger semantics) are best-effort.
