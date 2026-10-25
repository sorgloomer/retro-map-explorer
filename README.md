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

Requires Node.js and the Emscripten SDK (installed locally, nothing global):

```sh
git clone --depth 1 https://github.com/emscripten-core/emsdk .tools/emsdk
.tools/emsdk/emsdk install latest
.tools/emsdk/emsdk activate latest
node scripts/build.mjs          # -> dist/   (node scripts/build.mjs --debug for an unoptimised build)
node scripts/serve.mjs 8080     # http://localhost:8080
```

`scripts/serve.mjs` also serves `./assets` (put test copies of the games there) and exposes a **Dev assets** button
(or open `http://localhost:8080/?dev`) so you can try the app without dragging folders in.
Assets are only served to browsers holding the shared secret (cookie `rme_secret`). The server prints a link ending in `?secret=...` (secret from `RME_SECRET` or the generated, git-ignored `.dev-secret`); open it once and the app keeps the secret in a cookie. Share links include the secret when the browser has it, so only share them with people who may see your assets.
`dist/` contains only the app and can be hosted on any static server.

## Layout

```
src/vmath.h        vector / matrix math
src/inflate.*      DEFLATE/zlib decoder (no zlib dependency)
src/image.*        BMP + TGA decoders
src/vfs.*          file system over the dropped files (JS: list + ranged read, via Asyncify)
src/scene.h        game independent scene: triangle batches, textures, markers, lines
src/revolt.cpp     Re-Volt loader (.w .prm .fin .inf .fob .pan .taz .tri .lit .fld .rim)
src/carma.cpp      Carmageddon TDR 2000 loader (.dir/.pak archives, .hie, .mshs, .tga, .lins, .pup, .scol)
src/renderer.*     WebGL2 renderer (textured geometry, instanced markers, screen-space line ribbons)
src/app.cpp        camera, input, main loop, JS-facing API
web/               index.html, style.css, ui.js
```

## Adding a game

Implement `Game` (see `src/game.h`): `detect()`, `listLevels()` and `load()` which fills a `Scene`
(add triangles with `addTriangle` / `addIndexed`, annotations with `addCategory` + `addMarker` / `addLine` / `addBox`)
and register it in `initGames()` in `src/app.cpp`.

## Known gaps

* Carmageddon: animated water meshes use a different mesh layout and are skipped; dynamic props
  (moveables, trams, trains) are not placed; collision-only volumes are not shown.
* Re-Volt: AI nodes (`.fan`), camera nodes (`.cam`) and visiboxes (`.vis`) are not shown; objects other
  than pickups are only labelled with their numeric type.
* Format notes come from community documentation and reverse engineering; a few details (marker
  orientation, trigger semantics) are best-effort.
