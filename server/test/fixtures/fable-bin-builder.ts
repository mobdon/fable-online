import zlib from 'node:zlib';
import { fableHash } from '../../src/data/fable-bin.js';

/**
 * Builds tiny synthetic names.bin + game.bin pairs in Fable's layout, so the decoder and
 * extractor can be tested without any game files. All names and numbers here are made up.
 */
export type Field = { name: string | number; i32?: number; f32?: number; ref?: string; refs?: string[]; i32s?: number[] };

export interface DefSpec {
  group: string;
  name: string;
  own?: Field[];
  components?: { type: string; fields: Field[] }[];
}

export function buildBins(types: string[], defs: DefSpec[], chunkSize = 3): { game: Buffer; names: Buffer } {
  // names.bin
  const strings: string[] = [];
  const offsets = new Map<string, number>();
  let size = 0;
  const name = (s: string) => {
    if (!offsets.has(s)) {
      offsets.set(s, size + 4); // offset points at the string, after its 4-byte hash
      strings.push(s);
      size += 4 + Buffer.byteLength(s, 'latin1') + 1;
    }
    return offsets.get(s)!;
  };
  for (const t of types) {
    name(t);
    name(`NULLDEF_${t}`);
  }
  for (const d of defs) {
    name(d.group);
    name(d.name);
  }

  // entry index space: types, defs, components
  const comps: { type: string; owner: number; fields: Field[] }[] = [];
  defs.forEach((d, i) => d.components?.forEach((c) => comps.push({ type: c.type, owner: types.length + i, fields: c.fields })));
  const defIndex = new Map(defs.map((d, i) => [d.name, types.length + i]));
  const total = types.length + defs.length + comps.length;

  const fieldBytes = (fields: Field[]) =>
    Buffer.concat(
      fields.map((f) => {
        const h = Buffer.alloc(4);
        h.writeUInt32LE(typeof f.name === 'number' ? f.name : fableHash(f.name));
        let v: Buffer;
        if (f.i32s) {
          v = Buffer.alloc(4 + 4 * f.i32s.length);
          v.writeUInt32LE(f.i32s.length);
          f.i32s.forEach((n, i) => v.writeInt32LE(n, 4 + i * 4));
        } else if (f.refs) {
          v = Buffer.alloc(4 + 4 * f.refs.length);
          v.writeUInt32LE(f.refs.length);
          f.refs.forEach((r, i) => v.writeInt32LE(defIndex.get(r) ?? -1, 4 + i * 4));
        } else {
          v = Buffer.alloc(4);
          if (f.f32 !== undefined) v.writeFloatLE(f.f32);
          else v.writeInt32LE(f.ref !== undefined ? (defIndex.get(f.ref) ?? -1) : (f.i32 ?? 0));
        }
        return Buffer.concat([h, v]);
      }),
    );

  const entries: Buffer[] = [];
  for (let i = 0; i < types.length; i++) entries.push(Buffer.from([0, 0, 0]));
  let compCursor = types.length + defs.length;
  defs.forEach((d) => {
    const list = d.components ?? [];
    const head = Buffer.alloc(5 + 12 * list.length);
    head[0] = 1;
    head[2] = 1;
    head.writeUInt16LE(list.length, 3);
    list.forEach((c, j) => {
      head.writeUInt32LE(fableHash(c.type), 5 + j * 12);
      head.writeUInt32LE(compCursor++, 9 + j * 12);
      head.writeUInt32LE(defIndex.get(d.name)!, 13 + j * 12);
    });
    entries.push(Buffer.concat([head, fieldBytes(d.own ?? [])]));
  });
  for (const c of comps) entries.push(Buffer.concat([Buffer.from([1, 0, 1]), fieldBytes(c.fields)]));

  const namesCrc = 0x1234abcd;
  const nb = Buffer.alloc(20 + size);
  nb.writeUInt32LE(size, 0);
  nb.writeUInt32LE(namesCrc, 4);
  nb.writeUInt32LE(strings.length, 8);
  let o = 20;
  for (const s of strings) {
    nb.writeUInt32LE(fableHash(s), o);
    o += 4;
    o += nb.write(s, o, 'latin1') + 1;
  }

  const rows = Buffer.alloc(12 * total);
  types.forEach((t, i) => {
    rows.writeUInt32LE(name(t), i * 12);
    rows.writeUInt32LE(name(`NULLDEF_${t}`), i * 12 + 4);
    rows.writeUInt32LE(0, i * 12 + 8);
  });
  defs.forEach((d, j) => {
    const i = types.length + j;
    rows.writeUInt32LE(name(d.group), i * 12);
    rows.writeUInt32LE(name(d.name), i * 12 + 4);
    rows.writeUInt32LE(j + 1, i * 12 + 8);
  });
  comps.forEach((c, j) => {
    const i = types.length + defs.length + j;
    rows.writeUInt32LE(name(c.type), i * 12);
    rows.writeUInt32LE(0xffffffff, i * 12 + 4);
    rows.writeUInt32LE(j + 1, i * 12 + 8);
  });

  const chunks: Buffer[] = [];
  const table: [number, number][] = [];
  let off = 0;
  for (let first = 0; first < total; first += chunkSize) {
    const part = entries.slice(first, first + chunkSize);
    const head = Buffer.alloc(part.length * 2);
    let at = head.length;
    part.forEach((e, k) => {
      head.writeUInt16LE(at, k * 2);
      at += e.length;
    });
    const z = zlib.deflateSync(Buffer.concat([head, ...part]));
    table.push([first, off]);
    chunks.push(z);
    off += z.length;
  }
  table.push([total, off]);

  const header = Buffer.alloc(13);
  header[0] = 0;
  header.writeUInt32LE(0xfeed, 1);
  header.writeUInt32LE(namesCrc, 5);
  header.writeUInt32LE(total, 9);
  const t = Buffer.alloc(4 + table.length * 8 + 4);
  t.writeUInt32LE(table.length, 0);
  table.forEach(([a, b], i) => {
    t.writeUInt32LE(a, 4 + i * 8);
    t.writeUInt32LE(b, 8 + i * 8);
  });
  t.writeUInt32LE(off, 4 + table.length * 8);
  return { game: Buffer.concat([header, rows, t, ...chunks]), names: nb };
}
