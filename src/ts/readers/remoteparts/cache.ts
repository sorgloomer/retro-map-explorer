// The persistent cache of downloaded parts (IndexedDB). A cached part is valid exactly when its SHA-256 is the one the current
// parts.json lists, so after an archive changed only the parts whose hash changed are downloaded again.

const DB_NAME = 'rme-zip-cache', STORE = 'parts';

interface CachedPart {
  sha256: string;
  etag: string;
  data: ArrayBuffer;
}

export class PartsCache {
  private readonly db: IDBDatabase;

  private constructor(db: IDBDatabase) {
    this.db = db;
  }

  /** Opens the cache; null when IndexedDB is not available (the cache is an optimisation only). */
  static open(): Promise<PartsCache | null> {
    return new Promise((resolve) => {
      if (typeof indexedDB === 'undefined') { resolve(null); return; }
      try {
        const rq = indexedDB.open(DB_NAME, 2);
        rq.onupgradeneeded = () => {
          const db = rq.result;
          for (const name of Array.from(db.objectStoreNames)) db.deleteObjectStore(name);  // drop the 16 KiB chunk cache of older versions
          db.createObjectStore(STORE);
        };
        rq.onsuccess = () => resolve(new PartsCache(rq.result));
        rq.onerror = () => resolve(null);
      } catch { resolve(null); }
    });
  }

  private static key(folderUrl: string, index: number) {
    return folderUrl + '|' + index;
  }

  /** The cached bytes of part `index`, or null when it is missing or does not match the expected hash / length. */
  get(folderUrl: string, index: number, sha256: string, length: number): Promise<Uint8Array | null> {
    return new Promise((resolve) => {
      try {
        const rq = this.db.transaction(STORE, 'readonly').objectStore(STORE).get(PartsCache.key(folderUrl, index));
        rq.onsuccess = () => {
          const cached = rq.result as CachedPart | undefined;
          resolve(cached && cached.sha256 === sha256 && cached.data.byteLength === length ? new Uint8Array(cached.data) : null);
        };
        rq.onerror = () => resolve(null);
      } catch { resolve(null); }
    });
  }

  put(folderUrl: string, index: number, sha256: string, etag: string, data: ArrayBuffer): void {
    try {
      const cached: CachedPart = { sha256, etag, data };
      this.db.transaction(STORE, 'readwrite').objectStore(STORE).put(cached, PartsCache.key(folderUrl, index));
    } catch { /* the cache is an optimisation only */ }
  }
}
