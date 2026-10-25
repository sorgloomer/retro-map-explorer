// The abstractions of the zip reader: where an archive's bytes come from, and what comes out of it.

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
