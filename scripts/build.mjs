// Builds the app into ./dist:
//   * the WebAssembly module (rme.js / rme.wasm) with the project-local Emscripten SDK (.tools/emsdk)
//   * the web page: src/ts/*.ts is type-checked with tsc, then compiled, bundled and minified by rolldown into the single
//     file dist/app.js; index.html and style.css are copied next to it. Only these files are published, never the sources.
//   node scripts/build.mjs               optimised build
//   node scripts/build.mjs --debug       assertions, no optimisation (wasm), unminified bundle with a source map
//   node scripts/build.mjs --web-only    skip the WebAssembly build (fast iteration on the page)
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { rolldown } from 'rolldown';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const emsdk = path.join(root, '.tools', 'emsdk');
const debug = process.argv.includes('--debug');
const webOnly = process.argv.includes('--web-only');
const win = process.platform === 'win32';

const dist = path.join(root, 'dist');
fs.mkdirSync(dist, { recursive: true });

function buildWasm() {
  const emscriptenDir = path.join(emsdk, 'upstream', 'emscripten');
  const emxx = path.join(emscriptenDir, win ? 'em++.exe' : 'em++');
  if (!fs.existsSync(emxx)) {
    console.error('Emscripten not found. Install it into .tools/emsdk:\n' +
      '  git clone --depth 1 https://github.com/emscripten-core/emsdk .tools/emsdk\n' +
      '  .tools/emsdk/emsdk install latest && .tools/emsdk/emsdk activate latest');
    process.exit(1);
  }

  const env = { ...process.env, EMSDK: emsdk, EMSDK_QUIET: '1', EM_CONFIG: path.join(emsdk, '.emscripten') };
  env.PATH = [emscriptenDir, path.join(emsdk, 'upstream', 'bin'), process.env.PATH].join(path.delimiter);

  const sources = fs.readdirSync(path.join(root, 'src', 'cpp'), { recursive: true }).filter((f) => f.endsWith('.cpp')).map((f) => path.join(root, 'src', 'cpp', f));
  const args = [
    ...(debug ? ['-O0', '-g', '-sASSERTIONS=1'] : ['-O2']),
    '-std=c++17', '-Wall', '-Wno-unused-function', '-I' + path.join(root, 'src', 'cpp'),  // includes are relative to src/cpp
    '-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2',
    '-sASYNCIFY', '-sASYNCIFY_STACK_SIZE=131072',
    '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=256MB', '-sMAXIMUM_MEMORY=2GB', '-sSTACK_SIZE=2MB',
    '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8',
    '-sENVIRONMENT=web', '-sNO_EXIT_RUNTIME=1', '-sFILESYSTEM=0',
    ...sources,
    '-o', path.join(dist, 'rme.js'),
  ];

  const r = spawnSync(emxx, args, { env, stdio: 'inherit', shell: false });
  if (r.status !== 0) {
    console.error('em++ failed');
    process.exit(r.status || 1);
  }
}

// Type-check (tsc, no output), then bundle with rolldown. Everything lands in one minified script.
async function buildWeb() {
  const tsc = spawnSync(process.execPath, [path.join(root, 'node_modules', 'typescript', 'bin', 'tsc'), '-p', path.join(root, 'tsconfig.json')], { stdio: 'inherit' });
  if (tsc.status !== 0) {
    console.error('TypeScript errors: not bundling');
    process.exit(tsc.status || 1);
  }
  const bundle = await rolldown({ input: path.join(root, 'src', 'ts', 'main.ts'), platform: 'browser', logLevel: 'warn' });
  await bundle.write({ file: path.join(dist, 'app.js'), format: 'iife', minify: !debug, sourcemap: debug });
  await bundle.close();
  for (const f of ['index.html', 'style.css']) fs.copyFileSync(path.join(root, 'src', 'web', f), path.join(dist, f));
  // leftovers of older builds; the release build publishes nothing but the bundle (no source map: it would embed the sources)
  for (const stale of ['ui.js', 'remotezip.js', ...(debug ? [] : ['app.js.map'])]) fs.rmSync(path.join(dist, stale), { force: true });
}

if (!webOnly) buildWasm();
await buildWeb();
console.log('Built ->', dist);
