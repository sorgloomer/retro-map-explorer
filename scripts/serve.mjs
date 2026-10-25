// Tiny dev server: serves ./dist at "/" and the local ./assets folder at "/assets" (with Range support)
// plus a "/__manifest.json" listing of every asset file, so the app can be tried without drag & drop.
//   node scripts/serve.mjs [port]
// The assets are copyrighted game files, so they are only served to browsers that present a shared secret in the
// "rme_secret" cookie. The secret is RME_SECRET from the environment, else it is generated once into ./.dev-secret.
// Open the printed link once (?secret=...): the app stores it in the cookie and uses it for all asset requests.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dist = path.join(root, 'dist');
const assets = path.join(root, 'assets');
const port = Number(process.argv[2]) || 8080;

function loadSecret() {
  if (process.env.RME_SECRET) return process.env.RME_SECRET.trim();
  const f = path.join(root, '.dev-secret');
  try {
    const s = fs.readFileSync(f, 'utf8').trim();
    if (s) return s;
  } catch { /* first run */ }
  const s = crypto.randomBytes(18).toString('base64url');
  fs.writeFileSync(f, s + '\n');
  return s;
}
const secret = loadSecret();
function cookieOf(req, name) {
  for (const part of (req.headers.cookie || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0 && part.slice(0, i).trim() === name) {
      try { return decodeURIComponent(part.slice(i + 1).trim()); } catch { return ''; }
    }
  }
  return '';
}
function authorized(req) {
  const given = Buffer.from(cookieOf(req, 'rme_secret')), want = Buffer.from(secret);
  return given.length === want.length && crypto.timingSafeEqual(given, want);
}
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.json': 'application/json' };

function walk(dir, rel, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = rel ? rel + '/' + e.name : e.name;
    if (e.isDirectory()) {
      if (/^(fmv|music|sound|wavs|miles|__support|gallery|times)$/i.test(e.name)) continue;
      walk(path.join(dir, e.name), p, out);
    } else if (e.isFile()) out.push({ path: p, size: fs.statSync(path.join(dir, e.name)).size });
  }
}

function sendFile(req, res, file) {
  let st;
  try { st = fs.statSync(file); } catch { res.writeHead(404).end('not found'); return; }
  if (!st.isFile()) { res.writeHead(404).end('not found'); return; }
  const type = types[path.extname(file).toLowerCase()] || 'application/octet-stream';
  const range = /bytes=(\d*)-(\d*)/.exec(req.headers.range || '');
  if (range) {
    const start = range[1] ? Number(range[1]) : 0;
    const end = Math.min(range[2] ? Number(range[2]) : st.size - 1, st.size - 1);
    res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1, 'Accept-Ranges': 'bytes' });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': st.size, 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-cache' });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).pipe(res);
  }
}

http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  if ((url === '/__manifest.json' || url.startsWith('/assets/')) && !authorized(req)) {
    res.writeHead(401, { 'Content-Type': 'text/plain' }).end('assets need the shared secret (open the link printed by the server)');
    return;
  }
  if (url === '/__manifest.json') {
    if (!fs.existsSync(assets)) { res.writeHead(404).end(); return; }
    const files = [];
    walk(assets, '', files);
    res.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify({ files }));
    return;
  }
  const base = url.startsWith('/assets/') ? assets : dist;
  const rel = url.startsWith('/assets/') ? url.slice(8) : url === '/' ? 'index.html' : url.slice(1);
  const file = path.normalize(path.join(base, rel));
  if (!file.startsWith(base)) { res.writeHead(403).end(); return; }
  sendFile(req, res, file);
}).listen(port, () => console.log(`http://localhost:${port}/?dev&secret=${encodeURIComponent(secret)}\n(open this link once; the secret is kept in a cookie. Stored in .dev-secret or RME_SECRET)`));
