// Where the game files come from: a dropped / chosen folder, a zip file, remembered folder handles (File System Access API)
// or the encrypted asset archives next to the app. All of them end up as a FileSet that the WebAssembly side reads from.
import { openRemoteZip, type ByteSource } from './remotezip.ts';

const SKIP_DIRS = new Set(['fmv', 'music', 'sound', 'wavs', 'miles', '__support', 'gallery', 'times']);
const skipped = (parts: string[]) => parts.some((x) => SKIP_DIRS.has(x.toLowerCase()));

interface FileEntry { size: number; source: ByteSource | Blob }

export class FileSet {
  private readonly map = new Map<string, FileEntry>();

  add(path: string, size: number, source: ByteSource | Blob): void {
    this.map.set(path, { size, source });
  }

  get count(): number {
    return this.map.size;
  }

  /** "path\tsize" per line: what the C++ side indexes. */
  list(): string {
    const out: string[] = [];
    for (const [p, e] of this.map) out.push(p + '\t' + e.size);
    return out.join('\n');
  }

  async read(path: string, off: number, len: number): Promise<ArrayBuffer> {
    const e = this.map.get(path);
    if (!e) throw new Error('no such file: ' + path);
    if ('read' in e.source) return e.source.read(off, len);
    return e.source.slice(off, off + len).arrayBuffer();
  }
}

// ---------------------------------------------------------------- dropped items (webkitGetAsEntry)
const readEntries = (reader: FileSystemDirectoryReader) => new Promise<FileSystemEntry[]>((res, rej) => reader.readEntries(res, rej));
const entryFile = (entry: FileSystemFileEntry) => new Promise<File>((res, rej) => entry.file(res, rej));

async function walkEntry(entry: FileSystemEntry, set: FileSet, prefix: string): Promise<void> {
  if (entry.isFile) {
    const file = await entryFile(entry as FileSystemFileEntry);
    set.add(prefix + entry.name, file.size, file);
  } else if (entry.isDirectory) {
    if (SKIP_DIRS.has(entry.name.toLowerCase())) return;
    const reader = (entry as FileSystemDirectoryEntry).createReader();
    const children: FileSystemEntry[] = [];
    for (;;) {
      const batch = await readEntries(reader);
      if (!batch.length) break;
      children.push(...batch);
    }
    const dirPrefix = prefix + entry.name + '/';
    for (let i = 0; i < children.length; i += 64)
      await Promise.all(children.slice(i, i + 64).map((c) => walkEntry(c, set, dirPrefix)));
  }
}

export async function setFromEntries(entries: FileSystemEntry[]): Promise<FileSet> {
  const set = new FileSet();
  for (const e of entries) await walkEntry(e, set, '');
  return set;
}

// ---------------------------------------------------------------- remembered folders (Chromium): a refresh can reopen the game
export const fsAccess = typeof window.showDirectoryPicker === 'function';

const idbOpen = () => new Promise<IDBDatabase>((res, rej) => {
  const r = indexedDB.open('rme', 1);
  r.onupgradeneeded = () => r.result.createObjectStore('kv');
  r.onsuccess = () => res(r.result);
  r.onerror = () => rej(r.error);
});

export async function idbGet(key: string): Promise<unknown> {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => {
      const q = db.transaction('kv').objectStore('kv').get(key);
      q.onsuccess = () => res(q.result);
      q.onerror = () => rej(q.error);
    });
  } catch { return undefined; }
}

async function idbSet(key: string, val: unknown): Promise<void> {
  try {
    const db = await idbOpen();
    await new Promise<void>((res, rej) => {
      const tx = db.transaction('kv', 'readwrite');
      tx.objectStore('kv').put(val, key);
      tx.oncomplete = () => res();
      tx.onerror = () => rej(tx.error);
    });
  } catch (e) { console.warn('could not persist folder handle', e); }
}

/** The folders the visitor has chosen (and that are stored for the next visit). */
export let handles: FileSystemDirectoryHandle[] = [];
export const setHandles = (list: FileSystemDirectoryHandle[]): void => { handles = list; };

export async function rememberHandle(h: FileSystemDirectoryHandle): Promise<void> {
  for (const x of handles) if (await x.isSameEntry(h).catch(() => false)) return;
  handles.push(h);
  await idbSet('handles', handles);
}

async function walkHandle(dir: FileSystemDirectoryHandle, set: FileSet, prefix: string): Promise<void> {
  const children: FileSystemHandle[] = [];
  for await (const [, h] of dir.entries()) children.push(h);
  for (let i = 0; i < children.length; i += 64) {
    await Promise.all(children.slice(i, i + 64).map(async (h) => {
      if (h.kind === 'file') {
        const f = await (h as FileSystemFileHandle).getFile();
        set.add(prefix + h.name, f.size, f);
      } else if (!SKIP_DIRS.has(h.name.toLowerCase())) {
        await walkHandle(h as FileSystemDirectoryHandle, set, prefix + h.name + '/');
      }
    }));
  }
}

export async function setFromHandles(list: FileSystemDirectoryHandle[]): Promise<FileSet> {
  const set = new FileSet();
  for (const h of list) await walkHandle(h, set, h.name + '/');
  return set;
}

// ---------------------------------------------------------------- <input type=file webkitdirectory>
export function setFromInput(files: FileList): FileSet {
  const set = new FileSet();
  for (const f of files) {
    const p = f.webkitRelativePath || f.name;
    if (skipped(p.split('/'))) continue;
    set.add(p, f.size, f);
  }
  return set;
}

// ---------------------------------------------------------------- .zip file (for devices that cannot pick folders, e.g. Android)
// Stored entries are read by range; deflated entries are inflated once with DecompressionStream and cached.
export async function setFromZip(file: File): Promise<FileSet> {
  const u16 = (d: DataView, o: number) => d.getUint16(o, true);
  const u32 = (d: DataView, o: number) => d.getUint32(o, true);
  const u64 = (d: DataView, o: number) => Number(d.getBigUint64(o, true));
  const tailLen = Math.min(file.size, 65557 + 22);
  const tail = new DataView(await file.slice(file.size - tailLen).arrayBuffer());
  let eocd = -1;
  for (let i = tailLen - 22; i >= 0; i--) if (u32(tail, i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('not a zip file');
  let cdSize = u32(tail, eocd + 12), cdOff = u32(tail, eocd + 16);
  const count = u16(tail, eocd + 10);
  if ((count === 0xffff || cdOff === 0xffffffff) && eocd >= 20 && u32(tail, eocd - 20) === 0x07064b50) {
    const z64Off = u64(tail, eocd - 20 + 8);
    const z = new DataView(await file.slice(z64Off, z64Off + 56).arrayBuffer());
    cdSize = u64(z, 40); cdOff = u64(z, 48);
  }
  const cd = new DataView(await file.slice(cdOff, cdOff + cdSize).arrayBuffer());
  const bytes = new Uint8Array(cd.buffer);
  const dec = new TextDecoder();
  const set = new FileSet();
  let p = 0;
  while (p + 46 <= cd.byteLength && u32(cd, p) === 0x02014b50) {
    const method = u16(cd, p + 10);
    let csize = u32(cd, p + 20), size = u32(cd, p + 24);
    const nameLen = u16(cd, p + 28), extraLen = u16(cd, p + 30), commentLen = u16(cd, p + 32);
    let local = u32(cd, p + 42);
    const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen)).replace(/\\/g, '/');
    for (let x = p + 46 + nameLen, end = x + extraLen; x + 4 <= end;) {  // zip64 extra field
      const id = u16(cd, x), len = u16(cd, x + 2);
      if (id === 1) {
        let q = x + 4;
        if (size === 0xffffffff) { size = u64(cd, q); q += 8; }
        if (csize === 0xffffffff) { csize = u64(cd, q); q += 8; }
        if (local === 0xffffffff) { local = u64(cd, q); }
      }
      x += 4 + len;
    }
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/') || skipped(name.split('/'))) continue;
    set.add(name, size, makeZipEntry(file, method, local, csize));
  }
  return set;
}

function makeZipEntry(file: File, method: number, local: number, csize: number): ByteSource {
  let dataStart = -1;
  let cache: ArrayBuffer | null = null;
  const start = async () => {
    if (dataStart < 0) {
      const h = new DataView(await file.slice(local, local + 30).arrayBuffer());
      dataStart = local + 30 + h.getUint16(26, true) + h.getUint16(28, true);
    }
    return dataStart;
  };
  return {
    async read(off, len) {
      const s = await start();
      if (method === 0) return file.slice(s + off, s + off + len).arrayBuffer();
      if (method !== 8) throw new Error('unsupported zip compression method ' + method);
      if (!cache) {
        const stream = file.slice(s, s + csize).stream().pipeThrough(new DecompressionStream('deflate-raw'));
        cache = await new Response(stream).arrayBuffer();
      }
      return cache.slice(off, off + len);
    },
  };
}

// ---------------------------------------------------------------- the encrypted asset archives
// AES-encrypted zip archives next to the app. Only the directory and the parts that are needed are downloaded (see
// remotezip.ts); the secret from localStorage is the password.
const ASSET_ARCHIVES: [url: string, prefix: string][] = [['assets/ctdr2k.zip', 'Carmageddon TDR2K/'], ['assets/rvlt.zip', 'Re-Volt/']];

export async function setFromAssetArchives(secret: string): Promise<FileSet> {
  const lists = await Promise.all(ASSET_ARCHIVES.map(([url, prefix]) => openRemoteZip('/' + url, secret, prefix)));
  const set = new FileSet();
  for (const list of lists)
    for (const e of list) {
      if (skipped(e.name.split('/'))) continue;
      set.add(e.name, e.size, e.source);
    }
  return set;
}
