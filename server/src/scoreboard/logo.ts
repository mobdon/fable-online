import jpeg from 'jpeg-js';
import { GameError } from '../errors.js';

/** Clan logos are stored and sent at this size. */
export const LOGO_SIZE = 64;
const MAX_INPUT_BYTES = 512 * 1024;

/** The game splits every reply on CR LF before it reads the JPEG bytes, so a logo must not contain one. */
export const hasCrLf = (b: Buffer) => b.indexOf('\r\n', 0, 'latin1') !== -1;

/**
 * Turn an uploaded JPEG into the logo the game downloads: a 64x64 baseline JPEG with no CR LF
 * byte pair anywhere in it. Different quality settings give different bytes, so one of them
 * almost always works; if not, the pixels are nudged by one level and the search repeats.
 */
export function makeLogo(input: Buffer): Buffer {
  if (input.length === 0 || input.length > MAX_INPUT_BYTES) throw new GameError('bad_logo', 'Logo must be a JPEG under 512 KB');
  let img: { width: number; height: number; data: Buffer | Uint8Array };
  try {
    img = jpeg.decode(input, { useTArray: true, formatAsRGBA: true, maxResolutionInMP: 16, maxMemoryUsageInMB: 128 });
  } catch {
    throw new GameError('bad_logo', 'Logo must be a JPEG');
  }
  const px = resize(img.data, img.width, img.height, LOGO_SIZE);
  for (let nudge = 0; nudge < 8; nudge++) {
    if (nudge > 0) for (let i = 0; i < px.length; i += 4) px[i + ((i >> 2) % 3)]! ^= 1;
    for (let q = 92; q >= 40; q -= 4) {
      const out = Buffer.from(jpeg.encode({ width: LOGO_SIZE, height: LOGO_SIZE, data: px }, q).data);
      if (!hasCrLf(out)) return out;
    }
  }
  throw new GameError('bad_logo', 'Could not encode this logo, try another picture');
}

/** Area-average resize to a square (crops the long side to keep the picture centred). */
function resize(src: Uint8Array, w: number, h: number, size: number): Uint8Array {
  const side = Math.min(w, h);
  const x0 = Math.floor((w - side) / 2);
  const y0 = Math.floor((h - side) / 2);
  const out = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) {
    const sy0 = y0 + Math.floor((y * side) / size);
    const sy1 = Math.max(sy0 + 1, y0 + Math.floor(((y + 1) * side) / size));
    for (let x = 0; x < size; x++) {
      const sx0 = x0 + Math.floor((x * side) / size);
      const sx1 = Math.max(sx0 + 1, x0 + Math.floor(((x + 1) * side) / size));
      let r = 0, g = 0, b = 0, n = 0;
      for (let yy = sy0; yy < sy1; yy++)
        for (let xx = sx0; xx < sx1; xx++) {
          const i = (yy * w + xx) * 4;
          r += src[i]!;
          g += src[i + 1]!;
          b += src[i + 2]!;
          n++;
        }
      const o = (y * size + x) * 4;
      out[o] = Math.round(r / n);
      out[o + 1] = Math.round(g / n);
      out[o + 2] = Math.round(b / n);
      out[o + 3] = 255;
    }
  }
  return out;
}
