// WinZip AES (AE-1 / AE-2) for the zip reader: the cipher itself (AES in WinZip's little-endian counter mode, implemented here
// because WebCrypto's AES-CTR counts big-endian), the key derivation with the password check, and the authentication code.
// PBKDF2 and HMAC come from WebCrypto.

// AES (encryption only; CTR mode needs nothing else)
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

/** Length of the salt that precedes the encrypted data for an AES strength (1 = 128 bit, 2 = 192, 3 = 256). */
export const aesSaltLength = (strength: number) => 4 + 4 * strength;
/** Bytes between the start of the entry data and the ciphertext: the salt and the two password verification bytes. */
export const aesHeaderLength = (strength: number) => aesSaltLength(strength) + 2;
/** Bytes of the authentication code that follows the ciphertext. */
export const AES_AUTH_LENGTH = 10;

async function pbkdf2(password: string, salt: Uint8Array, bytes: number): Promise<Uint8Array> {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveBits']);
  return new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', salt: salt as BufferSource, iterations: 1000, hash: 'SHA-1' }, key, bytes * 8));
}

export interface AesKeys {
  ctr: AesCtr;
  /** key of the authentication code */
  mac: Uint8Array;
}

/**
 * Derives the keys of an entry from the password and the entry's header (salt + 2 verification bytes) and checks the
 * password against the verification bytes. Throws "wrong secret" for a wrong password.
 */
export async function deriveAesKeys(password: string, strength: number, header: Uint8Array, entryName: string): Promise<AesKeys> {
  const saltLen = aesSaltLength(strength), keyLen = 8 + 8 * strength;
  const material = await pbkdf2(password, header.subarray(0, saltLen), 2 * keyLen + 2);
  if (material[2 * keyLen] !== header[saltLen] || material[2 * keyLen + 1] !== header[saltLen + 1])
    throw new Error('wrong secret: it does not open ' + entryName);
  return { ctr: new AesCtr(material.subarray(0, keyLen)), mac: material.slice(keyLen, 2 * keyLen) };
}

/** Checks the 10 byte authentication code (HMAC-SHA1 of the ciphertext) of a whole entry. */
export async function verifyAuthCode(keys: AesKeys, ciphertext: Uint8Array, tag: Uint8Array, entryName: string): Promise<void> {
  const hmacKey = await crypto.subtle.importKey('raw', keys.mac as BufferSource, { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
  const sig = new Uint8Array(await crypto.subtle.sign('HMAC', hmacKey, ciphertext as BufferSource));
  for (let i = 0; i < AES_AUTH_LENGTH; i++) if (sig[i] !== tag[i]) throw new Error('corrupt data (authentication failed) in ' + entryName);
}
