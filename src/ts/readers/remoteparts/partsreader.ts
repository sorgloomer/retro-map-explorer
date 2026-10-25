// Reads a file that is hosted as a folder of fixed-size parts (see scripts/chunk_assets.py), so it works on static hosts
// without Range or ETag support:
//
//   <url>/meta.json   {"etag": "...", "chunksize": 4194304, "totalsize": N, "partcount": M}
//   <url>/parts.json  {"parts": [{"name": "game000.bin", "sha256": "..."}, ...]}
//   <url>/game000.bin ... plain files (named after the archive), each downloaded as a whole and checked against its SHA-256
//
// A part is looked up in memory, then in the IndexedDB cache (cache.ts), and only then downloaded.
import { pushAll } from '../../utils/arrays.ts';
import { PartsCache } from './cache.ts';
import type { ByteReader } from '../types.ts';

const PARALLEL = 4;              // parts downloaded at the same time
const MEM_PARTS = 24;            // parts kept in memory (96 MiB at 4 MiB per part)

const toHex = (buf: ArrayBuffer) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join('');

interface PartInfo { name: string; sha256: string }

export class PartsReader implements ByteReader {
  readonly url: string;
  /** names of the parts downloaded from the network (for diagnostics / tests) */
  readonly requests: string[] = [];
  size = 0;
  etag = '';
  chunksize = 0;
  chunks: PartInfo[] = [];
  private cache: PartsCache | null = null;
  private readonly mem = new Map<number, Uint8Array>();                  // part index -> bytes (insertion order = recency)
  private readonly inflight = new Map<number, Promise<Uint8Array>>();    // part index -> its download

  private constructor(folderUrl: string) {
    this.url = folderUrl.replace(/\/+$/, '');
  }

  /** Opens the parts folder: reads meta.json and parts.json and opens the persistent cache. */
  static async open(folderUrl: string): Promise<PartsReader> {
    const reader = new PartsReader(folderUrl);
    await reader.init();
    return reader;
  }

  private async init(): Promise<void> {
    const get = async (name: string): Promise<any> => {
      const r = await fetch(this.url + '/' + name, { cache: 'no-store' });
      if (!r.ok) throw new Error('cannot read ' + this.url + '/' + name + ' (HTTP ' + r.status + ')');
      return r.json();
    };
    const [meta, list] = await Promise.all([get('meta.json'), get('parts.json')]);
    this.chunksize = Number(meta.chunksize);
    this.size = Number(meta.totalsize);
    this.etag = String(meta.etag || '');
    this.chunks = Array.isArray(list.parts) ? list.parts : [];
    const count = Math.ceil(this.size / this.chunksize);
    if (!(this.chunksize > 0) || !(this.size > 0) || Number(meta.partcount) !== count || this.chunks.length !== count ||
        this.chunks.some((c) => !c || typeof c.name !== 'string' || !/^[0-9a-f]{64}$/i.test(c.sha256 || '')))
      throw new Error('invalid part list in ' + this.url);
    this.cache = await PartsCache.open();
  }

  private partLength(i: number): number {
    return Math.min(this.chunksize, this.size - i * this.chunksize);
  }

  /** Bytes [off, off + len) (clamped to the file). */
  async read(off: number, len: number): Promise<Uint8Array> {
    len = Math.max(0, Math.min(len, this.size - off));
    if (len === 0) return new Uint8Array(0);
    const p0 = Math.floor(off / this.chunksize), p1 = Math.floor((off + len - 1) / this.chunksize);
    const parts: Uint8Array[] = [];
    for (let i = p0; i <= p1; i += PARALLEL) {
      const batch: Promise<Uint8Array>[] = [];
      for (let j = i; j <= Math.min(p1, i + PARALLEL - 1); j++) batch.push(this.getPart(j));
      pushAll(parts, await Promise.all(batch));
    }
    const out = new Uint8Array(len);
    for (let i = p0; i <= p1; i++) {
      const part = parts[i - p0], pStart = i * this.chunksize;
      const from = Math.max(off, pStart), to = Math.min(off + len, pStart + part.length);
      out.set(part.subarray(from - pStart, to - pStart), from - off);
    }
    return out;
  }

  private getPart(i: number): Promise<Uint8Array> {
    const hit = this.mem.get(i);
    if (hit) { this.mem.delete(i); this.mem.set(i, hit); return Promise.resolve(hit); }
    let p = this.inflight.get(i);
    if (!p) {
      p = this.load(i).then((bytes) => {
        this.inflight.delete(i);
        this.mem.set(i, bytes);
        if (this.mem.size > MEM_PARTS) this.mem.delete(this.mem.keys().next().value as number);
        return bytes;
      }, (err) => { this.inflight.delete(i); throw err; });
      this.inflight.set(i, p);
    }
    return p;
  }

  private async load(i: number): Promise<Uint8Array> {
    const info = this.chunks[i], want = this.partLength(i), sha256 = info.sha256.toLowerCase();
    const cached = await this.cache?.get(this.url, i, sha256, want);
    if (cached) return cached;
    this.requests.push(info.name);
    const r = await fetch(this.url + '/' + info.name, { cache: 'no-store' });
    if (!r.ok) throw new Error('cannot download ' + this.url + '/' + info.name + ' (HTTP ' + r.status + ')');
    const buf = await r.arrayBuffer();
    if (buf.byteLength !== want) throw new Error(info.name + ' has the wrong size (' + buf.byteLength + ' instead of ' + want + ' bytes)');
    if (toHex(await crypto.subtle.digest('SHA-256', buf)) !== sha256)
      throw new Error(info.name + ' is corrupt (SHA-256 does not match parts.json)');
    this.cache?.put(this.url, i, sha256, this.etag, buf);
    return new Uint8Array(buf);
  }
}
