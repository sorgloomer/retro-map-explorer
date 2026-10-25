// Tiny dev server: serves ./dist at "/" and the local ./assets folder at "/assets" as plain static files.
//   node scripts/serve.mjs [port]
// No Range, no ETag, no authentication: the game files in ./assets are AES-encrypted zip archives split into parts by
// scripts/chunk_assets.py (ctdr2k.zip.parts/, rvlt.zip.parts/). The web app downloads the parts it needs and decrypts them in
// the browser with the secret it keeps in localStorage (open the app once with #secret=... in the address bar).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dist = path.join(root, 'dist');
const assets = path.join(root, 'assets');
const port = Number(process.argv[2]) || 8080;
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.json': 'application/json' };

http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  const base = url.startsWith('/assets/') ? assets : dist;
  const rel = url.startsWith('/assets/') ? url.slice(8) : url === '/' ? 'index.html' : url.slice(1);
  const file = path.normalize(path.join(base, rel));
  if (!file.startsWith(base)) { res.writeHead(403).end(); return; }
  let st;
  try { st = fs.statSync(file); } catch { res.writeHead(404).end('not found'); return; }
  if (!st.isFile()) { res.writeHead(404).end('not found'); return; }
  res.writeHead(200, { 'Content-Type': types[path.extname(file).toLowerCase()] || 'application/octet-stream', 'Content-Length': st.size });
  if (req.method === 'HEAD') return res.end();
  fs.createReadStream(file).pipe(res);
}).listen(port, () => console.log(`http://localhost:${port}/#secret=<secret>  (static files from ./dist and ./assets)`));
