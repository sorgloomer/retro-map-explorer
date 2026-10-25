// Reads a WinZip-AES encrypted .zip that is hosted as a folder of fixed-size parts (see scripts/chunk_assets.py), so it works
// on static hosts without Range or ETag support:
//
//   <url>.parts/meta.json   {"etag": "...", "chunksize": 4194304, "totalsize": N, "partcount": M}
//   <url>.parts/parts.json  {"parts": [{"name": "game000.bin", "sha256": "..."}, ...]}
//   <url>.parts/game000.bin ... plain files (named after the archive), each downloaded as a whole and checked against its SHA-256
//
// Downloaded parts are cached in IndexedDB; a cached part is used as long as its SHA-256 equals the one in the current
// parts.json, so after the archive changed only the parts that differ are downloaded again. No dependencies: the cipher (AES
// in WinZip's little-endian counter mode) is implemented here, PBKDF2 / HMAC / SHA-256 / inflate come from the browser.
//
//   const entries = await openRemoteZip('/assets/game.zip', password, 'Prefix/');   // [{ name, size, source: { read(off, len) } }]
//
// Entries stored without compression are read by range (and decrypted by range: the counter mode is seekable). Deflated
// entries have to be read as a whole, verified, decrypted and inflated once; they are kept in a small cache.
// (Only erasable TypeScript syntax is used, so Node can also run this file directly, e.g. for tests.)

const PARALLEL = 4;              // parts downloaded at the same time
const MEM_PARTS = 24;            // parts kept in memory (96 MiB at 4 MiB per part)
const INFLATED_LIMIT = 256 * 1024 * 1024;

/** Where the bytes of a file come from: `read` returns bytes [off, off + len) (clamped to the file). */
export interface ByteSource {
  read(off: number, len: number): Promise<ArrayBuffer>;
}

/** A file inside an archive. */
export interface ArchiveEntry {
  name: string;
  size: number;
  source: ByteSource;
}

export type ArchiveEntries = ArchiveEntry[] & { reader: PartsReader };

// ---------------------------------------------------------------- AES (encryption only; CTR mode needs nothing else)
const SBOX = new Uint8Array(256);
const TE = [new Uint32Array(256), new Uint32Array(256), new Uint32Array(256), new Uint32Array(256)];
(function initTables() {
  const rotl8 = (x: number, s: number) => ((x << s) | (x >> (8 - s))) & 0xff;
  let p = 1, q = 1;
  do {
    p = (p ^ (p << 1) ^ (p & 0x80 ? 0x11b : 0)) & 0xff;  // p *= 3 in GF(2^8)
    q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff;    // q /= 3
    if (q & 0x80) q ^= 0x09;
    SBOX[p] = (q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63) & 0xff;
  } while (p !== 1);
  SBOX[0] = 0x63;
  for (let i = 0; i < 256; i++) {
    const s = SBOX[i], s2 = ((s << 1) ^ (s & 0x80 ? 0x11b : 0)) & 0xff, s3 = s2 ^ s;
    const w = ((s2 << 24) | (s << 16) | (s << 8) | s3) >>> 0;
    TE[0][i] = w;
    TE[1][i] = ((w >>> 8) | (w << 24)) >>> 0;
    TE[2][i] = ((w >>> 16) | (w << 16)) >>> 0;
    TE[3][i] = ((w >>> 24) | (w << 8)) >>> 0;
  }
})();

export class AesCtr {
  private readonly rk: Uint32Array;
  private readonly nr: number;

  /** key: 16, 24 or 32 bytes */
  constructor(key: Uint8Array) {
    const nk = key.length / 4, nr = nk + 6, total = 4 * (nr + 1);
    const w = new Uint32Array(total);
    for (let i = 0; i < nk; i++) w[i] = ((key[4 * i] << 24) | (key[4 * i + 1] << 16) | (key[4 * i + 2] << 8) | key[4 * i + 3]) >>> 0;
    const sub = (t: number) => ((SBOX[t >>> 24] << 24) | (SBOX[(t >>> 16) & 255] << 16) | (SBOX[(t >>> 8) & 255] << 8) | SBOX[t & 255]) >>> 0;
    let rcon = 1;
    for (let i = nk; i < total; i++) {
      let t = w[i - 1];
      if (i % nk === 0) {
        t = (sub(((t << 8) | (t >>> 24)) >>> 0) ^ (rcon << 24)) >>> 0;
        rcon = ((rcon << 1) ^ (rcon & 0x80 ? 0x11b : 0)) & 0xff;
      } else if (nk > 6 && i % nk === 4) t = sub(t);
      w[i] = (w[i - nk] ^ t) >>> 0;
    }
    this.rk = w;
    this.nr = nr;
  }

  /** Encrypts one block given as four big-endian words into `out` (four words). */
  block(s0: number, s1: number, s2: number, s3: number, out: Uint32Array): void {
    const rk = this.rk, nr = this.nr, T0 = TE[0], T1 = TE[1], T2 = TE[2], T3 = TE[3];
    s0 ^= rk[0]; s1 ^= rk[1]; s2 ^= rk[2]; s3 ^= rk[3];
    let k = 4;
    for (let r = 1; r < nr; r++, k += 4) {
      const t0 = T0[s0 >>> 24] ^ T1[(s1 >>> 16) & 255] ^ T2[(s2 >>> 8) & 255] ^ T3[s3 & 255] ^ rk[k];
      const t1 = T0[s1 >>> 24] ^ T1[(s2 >>> 16) & 255] ^ T2[(s3 >>> 8) & 255] ^ T3[s0 & 255] ^ rk[k + 1];
      const t2 = T0[s2 >>> 24] ^ T1[(s3 >>> 16) & 255] ^ T2[(s0 >>> 8) & 255] ^ T3[s1 & 255] ^ rk[k + 2];
      const t3 = T0[s3 >>> 24] ^ T1[(s0 >>> 16) & 255] ^ T2[(s1 >>> 8) & 255] ^ T3[s2 & 255] ^ rk[k + 3];
      s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    out[0] = ((SBOX[s0 >>> 24] << 24) | (SBOX[(s1 >>> 16) & 255] << 16) | (SBOX[(s2 >>> 8) & 255] << 8) | SBOX[s3 & 255]) ^ rk[k];
    out[1] = ((SBOX[s1 >>> 24] << 24) | (SBOX[(s2 >>> 16) & 255] << 16) | (SBOX[(s3 >>> 8) & 255] << 8) | SBOX[s0 & 255]) ^ rk[k + 1];
    out[2] = ((SBOX[s2 >>> 24] << 24) | (SBOX[(s3 >>> 16) & 255] << 16) | (SBOX[(s0 >>> 8) & 255] << 8) | SBOX[s1 & 255]) ^ rk[k + 2];
    out[3] = ((SBOX[s3 >>> 24] << 24) | (SBOX[(s0 >>> 16) & 255] << 16) | (SBOX[(s1 >>> 8) & 255] << 8) | SBOX[s2 & 255]) ^ rk[k + 3];
  }

  /**
   * WinZip counter mode: the counter block is a 128-bit little-endian integer that starts at 1 for the first block of the
   * data. `data` starts at block number `firstBlock` (0-based) of the entry; returns a new, decrypted array.
   */
  decrypt(data: Uint8Array, firstBlock: number): Uint8Array {
    const out = new Uint8Array(data.length), ks = new Uint32Array(4);
    for (let i = 0, blockNo = firstBlock + 1; i < data.length; i += 16, blockNo++) {
      const n = blockNo >>> 0;  // blocks of an entry never reach 2^32 (64 GiB)
      const w0 = (((n & 255) << 24) | (((n >>> 8) & 255) << 16) | (((n >>> 16) & 255) << 8) | (n >>> 24)) >>> 0;
      this.block(w0, 0, 0, 0, ks);
      const left = Math.min(16, data.length - i);
      for (let j = 0; j < left; j++) out[i + j] = data[i + j] ^ ((ks[j >> 2] >>> (24 - 8 * (j & 3))) & 255);
    }
    return out;
  }
}

// ---------------------------------------------------------------- reader over a folder of parts
const DB_NAME = 'rme-zip-cache', STORE = 'parts';

function openDb(): Promise<IDBDatabase | null> {
  return new Promise((resolve) => {
    if (typeof indexedDB === 'undefined') { resolve(null); return; }
    try {
      const rq = indexedDB.open(DB_NAME, 2);
      rq.onupgradeneeded = () => {
        const db = rq.result;
        for (const name of Array.from(db.objectStoreNames)) db.deleteObjectStore(name);  // drop the 16 KiB chunk cache of older versions
        db.createObjectStore(STORE);
      };
      rq.onsuccess = () => resolve(rq.result);
      rq.onerror = () => resolve(null);
    } catch { resolve(null); }
  });
}

const toHex = (buf: ArrayBuffer) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join('');

interface PartInfo { name: string; sha256: string }
interface CachedPart { sha256: string; etag: string; data: ArrayBuffer }

export class PartsReader {
  readonly url: string;
  /** names of the parts downloaded from the network (for diagnostics / tests) */
  readonly requests: string[] = [];
  size = 0;
  etag = '';
  chunksize = 0;
  chunks: PartInfo[] = [];
  private db: IDBDatabase | null = null;
  private readonly mem = new Map<number, Uint8Array>();                  // part index -> bytes (insertion order = recency)
  private readonly inflight = new Map<number, Promise<Uint8Array>>();    // part index -> its download

  constructor(folderUrl: string) {
    this.url = folderUrl.replace(/\/+$/, '');
  }

  async init(): Promise<void> {
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
    this.db = await openDb();
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
      parts.push(...(await Promise.all(batch)));
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
    const info = this.chunks[i], want = this.partLength(i), key = this.url + '|' + i;
    const db = this.db;
    if (db) {
      const cached = await new Promise<CachedPart | null>((resolve) => {
        try {
          const rq = db.transaction(STORE, 'readonly').objectStore(STORE).get(key);
          rq.onsuccess = () => resolve(rq.result as CachedPart | undefined ?? null);
          rq.onerror = () => resolve(null);
        } catch { resolve(null); }
      });
      // a cached part is valid exactly when its hash is the one the current parts.json lists
      if (cached && cached.sha256 === info.sha256.toLowerCase() && cached.data.byteLength === want) return new Uint8Array(cached.data);
    }
    this.requests.push(info.name);
    const r = await fetch(this.url + '/' + info.name, { cache: 'no-store' });
    if (!r.ok) throw new Error('cannot download ' + this.url + '/' + info.name + ' (HTTP ' + r.status + ')');
    const buf = await r.arrayBuffer();
    if (buf.byteLength !== want) throw new Error(info.name + ' has the wrong size (' + buf.byteLength + ' instead of ' + want + ' bytes)');
    if (toHex(await crypto.subtle.digest('SHA-256', buf)) !== info.sha256.toLowerCase())
      throw new Error(info.name + ' is corrupt (SHA-256 does not match parts.json)');
    if (db) {
      try {
        const cached: CachedPart = { sha256: info.sha256.toLowerCase(), etag: this.etag, data: buf };
        db.transaction(STORE, 'readwrite').objectStore(STORE).put(cached, key);
      } catch { /* the cache is an optimisation only */ }
    }
    return new Uint8Array(buf);
  }
}

// ---------------------------------------------------------------- zip directory + entries
const u16 = (d: DataView, o: number) => d.getUint16(o, true);
const u32 = (d: DataView, o: number) => d.getUint32(o, true);
const u64 = (d: DataView, o: number) => Number(d.getBigUint64(o, true));

async function pbkdf2(password: string, salt: Uint8Array, bytes: number): Promise<Uint8Array> {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveBits']);
  return new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', salt: salt as BufferSource, iterations: 1000, hash: 'SHA-1' }, key, bytes * 8));
}

interface AesInfo { strength: number; method: number }
interface AesKeys { ctr: AesCtr; mac: Uint8Array; cipherStart: number; cipherLen: number }

/** A file of the central directory. */
interface ZipEntry {
  name: string;
  local: number;
  csize: number;
  method: number;
  aes: AesInfo | null;
  dataStart?: number;
  aesKeys?: AesKeys;
}

class RemoteZipFile {
  private readonly reader: PartsReader;
  private readonly password: string;
  private readonly inflated = new Map<ZipEntry, Promise<Uint8Array>>();  // least recently used first
  private inflatedBytes = 0;

  constructor(reader: PartsReader, password: string) {
    this.reader = reader;
    this.password = password;
  }

  private async dataStart(e: ZipEntry): Promise<number> {
    if (e.dataStart === undefined) {
      const h = new DataView((await this.reader.read(e.local, 30)).slice().buffer);
      if (u32(h, 0) !== 0x04034b50) throw new Error('bad zip local header for ' + e.name);
      e.dataStart = e.local + 30 + u16(h, 26) + u16(h, 28);
    }
    return e.dataStart;
  }

  // AES setup of an entry: salt + password verifier precede the data; the 10 byte authentication code follows it.
  private async aesSetup(e: ZipEntry): Promise<AesKeys> {
    if (e.aesKeys) return e.aesKeys;
    const aes = e.aes as AesInfo;
    const start = await this.dataStart(e);
    const saltLen = 4 + 4 * aes.strength, keyLen = 8 + 8 * aes.strength;
    const head = await this.reader.read(start, saltLen + 2);
    const material = await pbkdf2(this.password, head.subarray(0, saltLen), 2 * keyLen + 2);
    if (material[2 * keyLen] !== head[saltLen] || material[2 * keyLen + 1] !== head[saltLen + 1]) throw new Error('wrong secret: it does not open ' + e.name);
    e.aesKeys = {
      ctr: new AesCtr(material.subarray(0, keyLen)),
      mac: material.slice(keyLen, 2 * keyLen),
      cipherStart: start + saltLen + 2,
      cipherLen: e.csize - saltLen - 2 - 10,
    };
    return e.aesKeys;
  }

  async readRange(e: ZipEntry, off: number, len: number): Promise<Uint8Array> {
    if (e.method === 0) {
      if (!e.aes) return this.reader.read((await this.dataStart(e)) + off, len);
      const k = await this.aesSetup(e);
      len = Math.max(0, Math.min(len, k.cipherLen - off));
      const a0 = off - (off % 16);  // the keystream works on whole 16 byte blocks
      const ct = await this.reader.read(k.cipherStart + a0, off + len - a0);
      return k.ctr.decrypt(ct, a0 / 16).subarray(off - a0, off - a0 + len);
    }
    const whole = await this.wholeEntry(e);
    return whole.subarray(off, off + len);
  }

  private wholeEntry(e: ZipEntry): Promise<Uint8Array> {
    let p = this.inflated.get(e);
    if (p) { this.inflated.delete(e); this.inflated.set(e, p); return p; }
    p = this.buildWhole(e);
    this.inflated.set(e, p);
    p.then((bytes) => {
      this.inflatedBytes += bytes.length;
      while (this.inflatedBytes > INFLATED_LIMIT && this.inflated.size > 1) {  // forget the least recently used entries
        const [old, op] = this.inflated.entries().next().value as [ZipEntry, Promise<Uint8Array>];
        if (old === e) break;
        this.inflated.delete(old);
        op.then((b) => { this.inflatedBytes -= b.length; });
      }
    }, () => this.inflated.delete(e));
    return p;
  }

  private async buildWhole(e: ZipEntry): Promise<Uint8Array> {
    let data: Uint8Array;
    if (e.aes) {
      const k = await this.aesSetup(e);
      const ct = await this.reader.read(k.cipherStart, k.cipherLen);
      const tag = await this.reader.read(k.cipherStart + k.cipherLen, 10);
      const hmacKey = await crypto.subtle.importKey('raw', k.mac as BufferSource, { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
      const sig = new Uint8Array(await crypto.subtle.sign('HMAC', hmacKey, ct as BufferSource));
      for (let i = 0; i < 10; i++) if (sig[i] !== tag[i]) throw new Error('corrupt data (authentication failed) in ' + e.name);
      data = k.ctr.decrypt(ct, 0);
    } else {
      data = await this.reader.read(await this.dataStart(e), e.csize);
    }
    if (e.method === 0) return data;
    if (e.method !== 8) throw new Error('unsupported zip compression method ' + e.method + ' in ' + e.name);
    const stream = new Blob([data as BlobPart]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
}

/** Opens the archive hosted as `url`.parts/ and returns its file entries; `prefix` is prepended to every name. */
export async function openRemoteZip(url: string, password: string, prefix = ''): Promise<ArchiveEntries> {
  const reader = new PartsReader(url + '.parts');
  await reader.init();
  const zip = new RemoteZipFile(reader, password || '');
  const tailLen = Math.min(reader.size, 65557 + 22);
  const tail = await reader.read(reader.size - tailLen, tailLen);
  const tv = new DataView(tail.buffer as ArrayBuffer, tail.byteOffset, tail.length);
  let eocd = -1;
  for (let i = tailLen - 22; i >= 0; i--) if (u32(tv, i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('not a zip file: ' + url);
  let cdSize = u32(tv, eocd + 12), cdOff = u32(tv, eocd + 16);
  const count = u16(tv, eocd + 10);
  if ((count === 0xffff || cdOff === 0xffffffff) && eocd >= 20 && u32(tv, eocd - 20) === 0x07064b50) {  // zip64
    const z = new DataView((await reader.read(u64(tv, eocd - 20 + 8), 56)).slice().buffer);
    cdSize = u64(z, 40); cdOff = u64(z, 48);
  }
  const cdBytes = await reader.read(cdOff, cdSize);
  const cd = new DataView(cdBytes.buffer as ArrayBuffer, cdBytes.byteOffset, cdBytes.length);
  const dec = new TextDecoder();
  const entries = [] as unknown as ArchiveEntries;
  for (let p = 0; p + 46 <= cd.byteLength && u32(cd, p) === 0x02014b50;) {
    const flag = u16(cd, p + 8);
    let method = u16(cd, p + 10), csize = u32(cd, p + 20), size = u32(cd, p + 24);
    const nameLen = u16(cd, p + 28), extraLen = u16(cd, p + 30), commentLen = u16(cd, p + 32);
    let local = u32(cd, p + 42);
    const name = dec.decode(cdBytes.subarray(p + 46, p + 46 + nameLen)).replace(/\\/g, '/');
    let aes: AesInfo | null = null;
    for (let x = p + 46 + nameLen, end = x + extraLen; x + 4 <= end;) {
      const id = u16(cd, x), len = u16(cd, x + 2);
      if (id === 1) {  // zip64 extended information
        let q = x + 4;
        if (size === 0xffffffff) { size = u64(cd, q); q += 8; }
        if (csize === 0xffffffff) { csize = u64(cd, q); q += 8; }
        if (local === 0xffffffff) local = u64(cd, q);
      } else if (id === 0x9901 && len >= 7) {  // WinZip AES: version, vendor "AE", strength (1..3), real compression method
        aes = { strength: cd.getUint8(x + 8), method: u16(cd, x + 9) };
      }
      x += 4 + len;
    }
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/')) continue;
    if ((flag & 1) && !aes && method !== 99) throw new Error('unsupported zip encryption in ' + name);
    if (method === 99) {
      if (!aes) throw new Error('bad AES entry ' + name);
      method = aes.method;
    }
    const e: ZipEntry = { name: prefix + name, local, csize, method, aes: (flag & 1) ? aes : null };
    entries.push({
      name: e.name,
      size,
      source: { read: async (off, len) => (await zip.readRange(e, off, len)).slice().buffer as ArrayBuffer },
    });
  }
  entries.reader = reader;
  return entries;
}
