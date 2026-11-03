/**
 * The block cipher Fable.exe uses for every online scoreboard message (Fable.exe 0x55E2C0).
 *
 * It is XTEA (32 cycles, 64-bit blocks) with a twist:
 *  - The 128-bit key comes from a text key: buf[i] = (i + 1) * key[len - i] (low byte),
 *    for i in 0..len-1 (so buf[0] is always 0), then k = [buf[3], buf[2], buf[1], buf[0]]
 *    as sign-extended bytes.
 *  - After every full block the key drifts: k0 += buf[p], k1 -= buf[p+1], k2 += buf[p+2],
 *    k3 -= buf[p+3], with p cycling through buf from index 4.
 *  - Encrypted output starts with the plaintext length (u32 LE). A trailing partial block
 *    is zero padded.
 *
 * Verified byte for byte against the exe's own routine run under an emulator.
 */

/** Session key for the login request and its reply (Fable.exe 0x0124DCC0). */
export const LOGIN_KEY = '4AF92F27-1892-42f1-AA23-AF405325D8E6';
/** Fixed "CD key" every retail PC copy sends; used for every request after login (Fable.exe 0x0124C58C). */
export const CDKEY = 'THIS-ISMY-VALI-DCDK-EYOH-YESI-TISM';

const DELTA = 0x9e3779b9;
const sx8 = (b: number) => (b > 127 ? b - 256 : b);

function deriveBuf(key: Buffer): number[] {
  const n = key.length;
  const out: number[] = [];
  for (let i = 0; i < n; i++) {
    const c = i === 0 ? 0 : key[n - i]!; // key[len] is the C string terminator
    out.push(((i + 1) * c) & 0xff);
  }
  return out;
}

function encBlock(v: [number, number], k: number[]): void {
  let v0 = v[0] >>> 0;
  let v1 = v[1] >>> 0;
  let sum = 0;
  for (let i = 0; i < 32; i++) {
    v0 = (v0 + (((((v1 << 4) ^ (v1 >>> 5)) + v1) >>> 0) ^ ((sum + k[sum & 3]!) >>> 0))) >>> 0;
    sum = (sum + DELTA) >>> 0;
    v1 = (v1 + (((((v0 << 4) ^ (v0 >>> 5)) + v0) >>> 0) ^ ((sum + k[(sum >>> 11) & 3]!) >>> 0))) >>> 0;
  }
  v[0] = v0;
  v[1] = v1;
}

function decBlock(v: [number, number], k: number[]): void {
  let v0 = v[0] >>> 0;
  let v1 = v[1] >>> 0;
  let sum = (DELTA * 32) >>> 0;
  for (let i = 0; i < 32; i++) {
    v1 = (v1 - (((((v0 << 4) ^ (v0 >>> 5)) + v0) >>> 0) ^ ((sum + k[(sum >>> 11) & 3]!) >>> 0))) >>> 0;
    sum = (sum - DELTA) >>> 0;
    v0 = (v0 - (((((v1 << 4) ^ (v1 >>> 5)) + v1) >>> 0) ^ ((sum + k[sum & 3]!) >>> 0))) >>> 0;
  }
  v[0] = v0;
  v[1] = v1;
}

function run(data: Buffer, key: string, decrypt: boolean): Buffer {
  const buf = deriveBuf(Buffer.from(key, 'latin1'));
  const n = buf.length;
  const k = [3, 2, 1, 0].map((i) => sx8(buf[i] ?? 0) >>> 0);
  let plainLen: number;
  let body: Buffer;
  const out: Buffer[] = [];
  if (decrypt) {
    if (data.length < 4) throw new Error('lhcrypt: message too short');
    plainLen = data.readInt32LE(0);
    if (plainLen > data.length + 10) plainLen = data.length - (data.length % 8); // the exe's own clamp
    if (plainLen < 0) throw new Error('lhcrypt: bad length');
    body = data.subarray(4);
  } else {
    plainLen = data.length;
    const len = Buffer.alloc(4);
    len.writeUInt32LE(plainLen);
    out.push(len);
    body = data;
  }
  const full = Math.floor(body.length / 8);
  let p = 4;
  const v: [number, number] = [0, 0];
  for (let b = 0; b < full; b++) {
    v[0] = body.readUInt32LE(b * 8);
    v[1] = body.readUInt32LE(b * 8 + 4);
    (decrypt ? decBlock : encBlock)(v, k);
    const o = Buffer.alloc(8);
    o.writeUInt32LE(v[0], 0);
    o.writeUInt32LE(v[1], 4);
    out.push(o);
    for (let j = 0; j < 4; j++) {
      if (p >= n) p = 0;
      const d = sx8(buf[p++]!);
      k[j] = (j % 2 === 0 ? k[j]! + d : k[j]! - d) >>> 0;
    }
  }
  const rem = body.length - full * 8;
  if (rem > 0) {
    const t = Buffer.alloc(8);
    body.copy(t, 0, full * 8);
    v[0] = t.readUInt32LE(0);
    v[1] = t.readUInt32LE(4);
    (decrypt ? decBlock : encBlock)(v, k);
    const o = Buffer.alloc(8);
    o.writeUInt32LE(v[0], 0);
    o.writeUInt32LE(v[1], 4);
    out.push(o);
  }
  const all = Buffer.concat(out);
  return decrypt ? all.subarray(0, Math.min(plainLen, all.length)) : all;
}

export const lhEncrypt = (plain: Buffer, key: string) => run(plain, key, false);
export const lhDecrypt = (cipher: Buffer, key: string) => run(cipher, key, true);
