// Reads a .zip archive through a ByteReader, so the same code serves a file on the device of the visitor and an encrypted
// archive hosted as a folder of cached parts:
//
//   openZip(new BlobReader(file))                                                              // a .zip the visitor chose or dropped
//   openZip(await PartsReader.open('/assets/game.zip.parts'), { password, prefix: 'Game/' })   // a remote archive
//
// It handles zip64, stored and deflated entries and WinZip AES encryption (AE-1 / AE-2). Entries stored without compression are
// read by range (and decrypted by range: the counter mode is seekable). Deflated entries have to be read as a whole, verified,
// decrypted and inflated once; they are kept in a small cache.
//
//   index.ts       this file: the zip directory and the entries
//   types.ts       ByteReader (where bytes come from), ArchiveEntry (what comes out)
//   blobreader.ts  ByteReader over a File on the device
//   partsreader.ts ByteReader over a remote folder of parts (download, hash check, memory cache)
//   cache.ts       the persistent IndexedDB cache of the parts
//   zipaes.ts      WinZip AES: cipher, key derivation, authentication
//
// No dependencies: PBKDF2 / HMAC / SHA-256 / inflate come from the browser. Only erasable TypeScript syntax is used, so Node
// can also run these files directly (e.g. for tests).
import { AES_AUTH_LENGTH, aesHeaderLength, deriveAesKeys, verifyAuthCode, type AesKeys } from './zipaes.ts';
import type { ArchiveEntry } from './types.ts';
import type { ByteReader } from '../readers/types.ts';

export { AesCtr } from './zipaes.ts';
export { BlobReader } from '../readers/blobreader.ts';
export { PartsReader } from '../readers/remoteparts/partsreader.ts';
export type { ArchiveEntry, ByteSource } from './types.ts';
export type { ByteReader } from '../readers/types.ts';

const INFLATED_LIMIT = 256 * 1024 * 1024;

/** The file entries of an archive; `reader` is the reader it was opened with (for diagnostics / tests). */
export type ArchiveEntries = ArchiveEntry[] & { reader: ByteReader };

export interface OpenOptions {
  /** the password of an AES encrypted archive */
  password?: string;
  /** prepended to every entry name */
  prefix?: string;
}

const u16 = (d: DataView, o: number) => d.getUint16(o, true);
const u32 = (d: DataView, o: number) => d.getUint32(o, true);
const u64 = (d: DataView, o: number) => Number(d.getBigUint64(o, true));

/** An ArrayBuffer with exactly these bytes: the underlying buffer when the array spans all of it, else a copy. */
const toArrayBuffer = (a: Uint8Array): ArrayBuffer =>
  (a.byteOffset === 0 && a.byteLength === a.buffer.byteLength ? a.buffer : a.slice().buffer) as ArrayBuffer;

interface AesInfo { strength: number; method: number }
/** The keys of an AES entry plus where its ciphertext lies in the archive. */
interface EntryKeys extends AesKeys { cipherStart: number; cipherLen: number }

/** A file of the central directory. */
interface ZipEntry {
  name: string;
  local: number;
  csize: number;
  method: number;
  aes: AesInfo | null;
  dataStart?: number;
  aesKeys?: EntryKeys;
}

class ZipArchive {
  private readonly reader: ByteReader;
  private readonly password: string;
  private readonly inflated = new Map<ZipEntry, Promise<Uint8Array>>();  // least recently used first
  private inflatedBytes = 0;

  constructor(reader: ByteReader, password: string) {
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
  private async aesSetup(e: ZipEntry): Promise<EntryKeys> {
    if (e.aesKeys) return e.aesKeys;
    if (!this.password) throw new Error('a password is needed to open ' + e.name);
    const strength = (e.aes as AesInfo).strength;
    const start = await this.dataStart(e);
    const header = await this.reader.read(start, aesHeaderLength(strength));
    const keys = await deriveAesKeys(this.password, strength, header, e.name);
    e.aesKeys = {
      ...keys,
      cipherStart: start + aesHeaderLength(strength),
      cipherLen: e.csize - aesHeaderLength(strength) - AES_AUTH_LENGTH,
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
      const tag = await this.reader.read(k.cipherStart + k.cipherLen, AES_AUTH_LENGTH);
      await verifyAuthCode(k, ct, tag, e.name);
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

/** Reads the directory of the archive and returns its file entries (directories are left out). */
export async function openZip(reader: ByteReader, options: OpenOptions = {}): Promise<ArchiveEntries> {
  const prefix = options.prefix || '';
  const zip = new ZipArchive(reader, options.password || '');
  const tailLen = Math.min(reader.size, 65557 + 22);
  const tail = await reader.read(reader.size - tailLen, tailLen);
  const tv = new DataView(tail.buffer as ArrayBuffer, tail.byteOffset, tail.length);
  let eocd = -1;
  for (let i = tailLen - 22; i >= 0; i--) if (u32(tv, i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('not a zip file');
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
    entries.push({ name: e.name, size, source: { read: async (off, len) => toArrayBuffer(await zip.readRange(e, off, len)) } });
  }
  entries.reader = reader;
  return entries;
}
