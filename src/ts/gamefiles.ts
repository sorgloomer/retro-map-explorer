// Where the game files come from: a dropped / chosen folder, a zip file, remembered folder handles (File System Access API)
// or the encrypted asset archives next to the app. All of them end up as a FileSet that the WebAssembly side reads from.
import { BlobReader, PartsReader, openZip, type ByteSource } from './zip/index.ts';
import { pushAll } from './utils/arrays.ts';

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
      pushAll(children, batch);
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
export async function setFromZip(file: File): Promise<FileSet> {
  const set = new FileSet();
  for (const e of await openZip(new BlobReader(file))) if (!skipped(e.name.split('/'))) set.add(e.name, e.size, e.source);
  return set;
}

// ---------------------------------------------------------------- the encrypted asset archives
// AES-encrypted zip archives next to the app. Only the directory and the parts that are needed are downloaded (see
// zip/); the secret from localStorage is the password.
const ASSET_ARCHIVES: [url: string, prefix: string][] = [['assets/ctdr2k.zip', 'Carmageddon TDR2K/'], ['assets/rvlt.zip', 'Re-Volt/']];

export async function setFromAssetArchives(secret: string): Promise<FileSet> {
  const lists = await Promise.all(ASSET_ARCHIVES.map(async ([url, prefix]) => openZip(await PartsReader.open('/' + url + '.parts'), { password: secret, prefix })));
  const set = new FileSet();
  for (const list of lists)
    for (const e of list) {
      if (skipped(e.name.split('/'))) continue;
      set.add(e.name, e.size, e.source);
    }
  return set;
}
