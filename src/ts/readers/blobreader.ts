// A ByteReader over a Blob / File on the device of the visitor. Nothing to cache: the browser serves the ranges from disk.
import type { ByteReader } from './types.ts';

export class BlobReader implements ByteReader {
  readonly size: number;
  private readonly blob: Blob;

  constructor(blob: Blob) {
    this.blob = blob;
    this.size = blob.size;
  }

  async read(off: number, len: number): Promise<Uint8Array> {
    len = Math.max(0, Math.min(len, this.size - off));
    return new Uint8Array(await this.blob.slice(off, off + len).arrayBuffer());
  }
}
