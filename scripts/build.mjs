// Builds the app into ./dist:
//   * the WebAssembly module (rme.js / rme.wasm) with the project-local Emscripten SDK (.tools/emsdk)
//   * the web page: src/ts/*.ts is type-checked with tsc, then compiled, bundled and minified by rolldown into the single
//     file dist/app.js; index.html and style.css are copied next to it. Only these files are published, never the sources.
//   node scripts/build.mjs               optimised build
//   node scripts/build.mjs --debug       assertions, no optimisation (wasm), unminified bundle with a source map
//   node scripts/build.mjs --web-only    skip the WebAssembly build (fast iteration on the page)
//   node scripts/build.mjs --legacy-physics  the car runs on the engine's own physics (the default is Box3D, see minigame01/box3d_vehicle.h)
// RME_TOOLS_DIR (default .tools) and RME_DIST_DIR (default dist) move the tools and the output (the container of docker-compose.yml uses them). It installs what it needs the first time: the Node packages, Emscripten, Box3D.
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import crypto from 'node:crypto';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// where the tools are (Emscripten, Box3D, the compiled objects) and where the output goes: the project's .tools and dist, unless the
// environment says otherwise (the container of docker-compose.yml keeps the tools in a volume, and the sources are read-only)
const toolsDir = process.env.RME_TOOLS_DIR ? path.resolve(process.env.RME_TOOLS_DIR) : path.join(root, '.tools');
const emsdk = path.join(toolsDir, 'emsdk');
const debug = process.argv.includes('--debug');
const webOnly = process.argv.includes('--web-only');
const legacyPhysics = process.argv.includes('--legacy-physics');
const win = process.platform === 'win32';

const dist = process.env.RME_DIST_DIR ? path.resolve(process.env.RME_DIST_DIR) : path.join(root, 'dist');
fs.mkdirSync(dist, { recursive: true });

// Box3D (https://github.com/erincatto/box3d, MIT): C17 sources, cloned into .tools/lib/box3d at the commit this project was made with.
const BOX3D_COMMIT = '336263320469e5e9febfe18697ab562488f7e97a';
const box3dDir = path.join(toolsDir, 'lib', 'box3d');
function ensureBox3d() {
  if (fs.existsSync(path.join(box3dDir, 'include', 'box3d', 'box3d.h'))) return;
  console.log('fetching Box3D', BOX3D_COMMIT);
  fs.mkdirSync(box3dDir, { recursive: true });
  const git = (...args) => {
    const r = spawnSync('git', args, { cwd: box3dDir, stdio: 'inherit' });
    if (r.status !== 0) { console.error('git failed'); process.exit(r.status || 1); }
  };
  git('init', '-q');
  git('remote', 'add', 'origin', 'https://github.com/erincatto/box3d.git');
  git('fetch', '--depth', '1', 'origin', BOX3D_COMMIT);
  git('checkout', '-q', 'FETCH_HEAD');
}

// Emscripten: cloned into <tools>/emsdk and installed there when em++ is not there yet (or when a stamp left by an earlier install says it
// was another version; an emsdk that was put there by hand, with no stamp, is left as it is).
const EMSDK_VERSION = '6.0.13';   // the Emscripten the project was made with
function ensureEmscripten(emxx) {
  const stamp = path.join(emsdk, '.rme-version');
  const stamped = fs.existsSync(stamp) ? fs.readFileSync(stamp, 'utf8').trim() : null;
  if (fs.existsSync(emxx) && (stamped === null || stamped === EMSDK_VERSION)) return;
  console.log('installing Emscripten', EMSDK_VERSION, 'into', emsdk);
  const run = (cmd, args, cwd) => {
    const r = spawnSync(cmd, args, { cwd, stdio: 'inherit', shell: win });
    if (r.status !== 0) { console.error(cmd + ' failed'); process.exit(r.status || 1); }
  };
  if (!fs.existsSync(path.join(emsdk, '.git'))) {
    fs.mkdirSync(emsdk, { recursive: true });
    run('git', ['clone', '--depth', '1', 'https://github.com/emscripten-core/emsdk', emsdk], toolsDir);
  }
  const emsdkCommand = win ? 'emsdk.bat' : './emsdk';
  run(emsdkCommand, ['install', EMSDK_VERSION], emsdk);
  run(emsdkCommand, ['activate', EMSDK_VERSION], emsdk);
  fs.writeFileSync(stamp, EMSDK_VERSION + '\n');
}

// The Node packages (the bundler is one of them), installed when node_modules is not there or was installed from another lock file. The
// sources may be read-only (the container): npm ci only writes node_modules, which may be a volume of its own.
function ensureNodePackages() {
  const lockFile = path.join(root, 'package-lock.json');
  const hasLock = fs.existsSync(lockFile);
  const hash = crypto.createHash('sha256').update(fs.readFileSync(hasLock ? lockFile : path.join(root, 'package.json'))).digest('hex');
  const stamp = path.join(root, 'node_modules', '.rme-lock-hash');
  if (fs.existsSync(stamp) && fs.readFileSync(stamp, 'utf8').trim() === hash) return;
  if (!fs.existsSync(path.join(root, 'package.json'))) {
    console.error('no package.json in ' + root + ': is the project mounted? In it:', fs.readdirSync(root).join(' '));
    process.exit(1);
  }
  console.log('installing the Node packages');
  const r = spawnSync('npm', hasLock ? ['ci', '--no-audit', '--no-fund'] : ['install', '--no-package-lock', '--no-audit', '--no-fund'], { cwd: root, stdio: 'inherit', shell: win });
  if (r.status !== 0) { console.error('npm failed'); process.exit(r.status || 1); }
  fs.writeFileSync(stamp, hash + '\n');
}

async function buildWasm() {
  const emscriptenDir = path.join(emsdk, 'upstream', 'emscripten');
  const emxx = path.join(emscriptenDir, win ? 'em++.exe' : 'em++');
  const emcc = path.join(emscriptenDir, win ? 'emcc.exe' : 'emcc');
  ensureEmscripten(emxx);

  const env = { ...process.env, EMSDK: emsdk, EMSDK_QUIET: '1', EM_CONFIG: path.join(emsdk, '.emscripten') };
  env.PATH = [emscriptenDir, path.join(emsdk, 'upstream', 'bin'), process.env.PATH].join(path.delimiter);

  // Every source is compiled to its own object file, all at once (one process per core); a source is compiled again only when it or
  // a header it includes is newer than its object, or the flags changed. Then the objects are linked: the one step that is always run.
  const srcDir = path.join(root, 'src', 'cpp');
  const objDir = path.join(emsdk, 'objects', (debug ? 'debug' : 'release') + (legacyPhysics ? '-legacy' : ''));  // (inside .tools: not published, not in git)
  fs.mkdirSync(objDir, { recursive: true });
  const sources = fs.readdirSync(srcDir, { recursive: true }).filter((f) => f.endsWith('.cpp'));
  const optimisation = debug ?['-O0', '-g', '-sASSERTIONS=1'] : ['-O2'];
  if (!legacyPhysics) ensureBox3d();
  const box3dInclude = ['-I' + path.join(box3dDir, 'include')];
  const compileFlags = [...optimisation, '-std=c++17', '-Wall', '-Wno-unused-function', '-I' + srcDir,  // includes are relative to src/cpp
    ...(legacyPhysics ? ['-DRME_PHYSICS_LEGACY'] : box3dInclude)];
  // Box3D is C17; SIMD: its wasm path is the SSE2 one, on 128 bit wasm vectors; no fused multiply-add (its results do not depend on the target)
  const box3dFlags = [...optimisation, '-std=gnu17', '-DNDEBUG', '-msimd128', '-msse2', '-ffp-contract=off', ...box3dInclude, '-I' + path.join(box3dDir, 'src')];
  const flagsFile = path.join(objDir, 'flags.txt');
  const flagsText = compileFlags.join(' ') + '\n' + box3dFlags.join(' ');
  const flagsChanged = !fs.existsSync(flagsFile) || fs.readFileSync(flagsFile, 'utf8') !== flagsText;
  fs.writeFileSync(flagsFile, flagsText);

  const mtime = (f) => (fs.existsSync(f) ? fs.statSync(f).mtimeMs : 0);
  // the files a compilation read, from the dependency list the compiler wrote (Makefile syntax: "obj: dep dep \ dep")
  const dependencies = (depFile) => {
    if (!fs.existsSync(depFile)) return null;
    const joined = fs.readFileSync(depFile, 'utf8').split('\\\n').join(' ');
    const text = joined.slice(joined.indexOf(': ') + 1);  // (not the first colon: a Windows path has one)
    return text.split(/(?<!\\)\s+/).filter(Boolean).map((d) => d.split('\\ ').join(' '));
  };
  const isCurrent = (src, obj, depFile) => {
    if (flagsChanged || !fs.existsSync(obj)) return false;
    const deps = dependencies(depFile);
    return deps !== null && deps.every((d) => mtime(d) <= mtime(obj));
  };

  const allJobs = sources.map((f) => {
    const name = f.split(path.sep).join('_');
    return { src: path.join(srcDir, f), obj: path.join(objDir, name + '.o'), dep: path.join(objDir, name + '.d'), name: f, compiler: emxx, flags: compileFlags };
  });
  if (!legacyPhysics) {
    for (const f of fs.readdirSync(path.join(box3dDir, 'src')).filter((n) => n.endsWith('.c'))) {
      allJobs.push({ src: path.join(box3dDir, 'src', f), obj: path.join(objDir, 'box3d_' + f + '.o'), dep: path.join(objDir, 'box3d_' + f + '.d'), name: 'box3d/' + f, compiler: emcc, flags: box3dFlags });
    }
  }
  const jobs = allJobs.filter((j) => !isCurrent(j.src, j.obj, j.dep));
  const started = Date.now();
  console.log(`compiling ${jobs.length} of ${allJobs.length} files on ${os.availableParallelism()} cores`);
  const runCompile = (job) => new Promise((resolve) => {
    const child = spawn(job.compiler, [...job.flags, '-MMD', '-MF', job.dep, '-c', job.src, '-o', job.obj], { env, stdio: 'inherit', shell: false });
    child.on('close', (code) => resolve(code === 0));
  });
  const queue = [...jobs];
  let failed = false;
  await Promise.all(Array.from({ length: Math.min(os.availableParallelism(), jobs.length) }, async () => {
    while (queue.length && !failed) if (!(await runCompile(queue.shift()))) failed = true;
  }));
  if (failed) {
    console.error('em++ failed');
    process.exit(1);
  }
  const compiled = Date.now();

  const linkArgs = [
    ...optimisation,
    '-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2',
    '-sASYNCIFY', '-sASYNCIFY_STACK_SIZE=131072',
    '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=256MB', '-sMAXIMUM_MEMORY=2GB', '-sSTACK_SIZE=2MB',
    '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8',
    '-sENVIRONMENT=web', '-sNO_EXIT_RUNTIME=1', '-sFILESYSTEM=0',
    ...allJobs.map((j) => j.obj),
    '-o', path.join(dist, 'rme.js'),
  ];
  const r = spawnSync(emxx, linkArgs, { env, stdio: 'inherit', shell: false });
  if (r.status !== 0) {
    console.error('em++ failed');
    process.exit(r.status || 1);
  }
  console.log(`wasm: compile ${((compiled - started) / 1000).toFixed(1)} s, link ${((Date.now() - compiled) / 1000).toFixed(1)} s`);
}

// Type-check (tsc, no output), then bundle with rolldown. Everything lands in one minified script.
async function buildWeb() {
  const tsc = spawnSync(process.execPath, [path.join(root, 'node_modules', 'typescript', 'bin', 'tsc'), '-p', path.join(root, 'tsconfig.json')], { stdio: 'inherit' });
  if (tsc.status !== 0) {
    console.error('TypeScript errors: not bundling');
    process.exit(tsc.status || 1);
  }
  const { rolldown } = await import('rolldown');
  const bundle = await rolldown({ input: path.join(root, 'src', 'ts', 'main.tsx'), platform: 'browser', logLevel: 'warn' });
  await bundle.write({ file: path.join(dist, 'app.js'), format: 'iife', minify: !debug, sourcemap: debug });
  await bundle.close();
  for (const f of ['index.html', 'style.css']) fs.copyFileSync(path.join(root, 'src', 'web', f), path.join(dist, f));
  // leftovers of older builds; the release build publishes nothing but the bundle (no source map: it would embed the sources)
  for (const stale of ['ui.js', 'remotezip.js', ...(debug ? [] : ['app.js.map'])]) fs.rmSync(path.join(dist, stale), { force: true });
}

ensureNodePackages();
if (!webOnly) await buildWasm();
await buildWeb();
console.log('Built ->', dist);

