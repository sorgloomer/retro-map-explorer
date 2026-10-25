// Builds the WebAssembly app into ./dist using the project-local Emscripten SDK (.tools/emsdk).
//   node scripts/build.mjs           optimised build
//   node scripts/build.mjs --debug   assertions, no optimisation
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const emsdk = path.join(root, '.tools', 'emsdk');
const debug = process.argv.includes('--debug');
const win = process.platform === 'win32';

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

const dist = path.join(root, 'dist');
fs.mkdirSync(dist, { recursive: true });

const sources = fs.readdirSync(path.join(root, 'src')).filter((f) => f.endsWith('.cpp')).map((f) => path.join(root, 'src', f));
const args = [
  ...(debug ? ['-O0', '-g', '-sASSERTIONS=1'] : ['-O2']),
  '-std=c++17', '-Wall', '-Wno-unused-function',
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

fs.cpSync(path.join(root, 'web'), dist, { recursive: true });
console.log('Built ->', dist);
