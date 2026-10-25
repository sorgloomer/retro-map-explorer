/** Random access to the bytes of an archive (a file on the device, or a remote archive with its download cache). */
export interface ByteReader {
    readonly size: number;
    /** Bytes [off, off + len), clamped to the end of the archive. */
    read(off: number, len: number): Promise<Uint8Array>;
}
