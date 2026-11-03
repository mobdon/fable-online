import fs from 'node:fs';

/**
 * Reader for data/Levels/FinalAlbion.wad, the "BBBB" archive that holds every .lev and .tng.
 * Same layout as tools/extractor/spawns.mjs: a 32-byte header (entry count at 20, index offset
 * at 28), then an index of variable-length rows at the end of the file.
 */
export interface WadEntry {
  name: string;
  offset: number;
  size: number;
}

export function readWadIndex(fd: number): WadEntry[] {
  const head = Buffer.alloc(32);
  fs.readSync(fd, head, 0, 32, 0);
  if (head.toString('latin1', 0, 4) !== 'BBBB') throw new Error('not a Fable .wad');
  const count = head.readUInt32LE(20);
  const entriesOffset = head.readUInt32LE(28);
  const size = fs.fstatSync(fd).size - entriesOffset;
  const buf = Buffer.alloc(size);
  fs.readSync(fd, buf, 0, size, entriesOffset);
  const out: WadEntry[] = [];
  let o = 0;
  for (let i = 0; i < count; i++) {
    o += 16 + 8; // unknown[16], id, unknown
    const len = buf.readUInt32LE(o);
    const off = buf.readUInt32LE(o + 4);
    const pathLen = buf.readUInt32LE(o + 12);
    o += 16;
    const name = buf.toString('latin1', o, o + pathLen);
    o += pathLen + 16 + 28 + 28 + 20; // unknown[16], created, accessed, written
    out.push({ name, offset: off, size: len });
  }
  return out;
}

/** Every .tng in the wad, as text, keyed by its base name without extension. */
export function readTngFiles(wadPath: string): Map<string, string> {
  const fd = fs.openSync(wadPath, 'r');
  const out = new Map<string, string>();
  try {
    for (const e of readWadIndex(fd)) {
      if (!e.name.toLowerCase().endsWith('.tng')) continue;
      const buf = Buffer.alloc(e.size);
      fs.readSync(fd, buf, 0, e.size, e.offset);
      out.set(e.name.replace(/\\/g, '/').replace(/^.*\//, '').replace(/\.tng$/i, ''), buf.toString('latin1'));
    }
  } finally {
    fs.closeSync(fd);
  }
  return out;
}

/** FinalAlbion.wld maps each level file to its LevelScriptName, which is the server's region key. */
export function levelScriptNames(wldText: string): Map<string, string> {
  const out = new Map<string, string>();
  for (const block of wldText.split(/\bNewMap\s+/).slice(1)) {
    const lev = block.match(/\bLevelName\s+"?([^";]+)"?;/)?.[1];
    const script = block.match(/\bLevelScriptName\s+"?([^";]+)"?;/)?.[1];
    if (lev && script) out.set(lev.replace(/\\/g, '/').replace(/^.*\//, '').replace(/\.lev$/i, '').toLowerCase(), script);
  }
  return out;
}

/** One `NewThing <kind>; ... EndThing;` block. */
export interface TngThing {
  kind: string;
  body: string;
}

export function tngThings(text: string): TngThing[] {
  return [...text.matchAll(/NewThing (\w+);([\s\S]*?)EndThing;/g)].map((m) => ({ kind: m[1]!, body: m[2]! }));
}

export const tngNum = (body: string, key: string): number | undefined => {
  const m = body.match(new RegExp(`\\b${key}\\s+(-?[0-9.eE+-]+);`));
  return m ? Number(m[1]) : undefined;
};

export const tngStr = (body: string, key: string): string | undefined => body.match(new RegExp(`\\b${key}\\s+"?([^";]*)"?;`))?.[1];

export const tngBool = (body: string, key: string): boolean | undefined => {
  const v = tngStr(body, key);
  return v === undefined ? undefined : v.toUpperCase() === 'TRUE';
};

/** Position and facing of a thing (level-local metres, z up; yaw 0 = facing +y). */
export function tngPlace(body: string): { x: number; y: number; z: number; yaw: number } {
  const fx = tngNum(body, 'RHSetForwardX') ?? 0;
  const fy = tngNum(body, 'RHSetForwardY') ?? 1;
  return {
    x: tngNum(body, 'PositionX') ?? 0,
    y: tngNum(body, 'PositionY') ?? 0,
    z: tngNum(body, 'PositionZ') ?? 0,
    yaw: Math.round(Math.atan2(-fx, fy) * 1000) / 1000 + 0,
  };
}
