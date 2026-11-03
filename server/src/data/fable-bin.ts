import zlib from 'node:zlib';

/**
 * Reader for Fable: The Lost Chapters compiled defs (data/CompiledDefs/game.bin + names.bin).
 *
 * Nothing from these files is shipped. The server's setup step runs this against the
 * operator's own install and writes the numbers it needs to server/data/game-data.json.
 *
 * names.bin
 *   u32 size, u32 crc, u32 count, u32 ?, u32 0, then `count` x { u32 hash, char name[] \0 }.
 *   Other files refer to a name by its byte offset counted from the end of the 20-byte header.
 *
 * game.bin
 *   u8 0 (use names.bin), u32 file id, u32 names.bin crc, u32 entryCount
 *   entryCount x 12-byte rows {u32 a, u32 b, u32 c}, in three runs:
 *     def types   { typeName, NULLDEF_typeName, 0 }       entry = the type's default values
 *     defs        { groupName, defName, indexInGroup }    entry = component list + own fields
 *     components  { typeName, 0xFFFFFFFF, indexInType }   entry = that component's fields
 *   u32 chunkCount, chunkCount x { u32 firstEntry, u32 offset }  (last row is a terminator)
 *   u32 compressedSize, then zlib chunks. A chunk starts with u16 offsets, one per entry.
 *
 * Every entry is 3 header bytes, then fields as { u32 crc32(name), value }. A def entry
 * instead has a u16 component count and 12-byte { u32 typeHash, u32 entryIndex, u32 ownerDef }
 * rows first. Def references inside values are entry indexes (types + defs + components).
 * Field names are hashed with plain reflected CRC-32 (poly 0xEDB88320, init 0, no final xor).
 */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

/** Fable's name hash. */
export function fableHash(name: string): number {
  let c = 0;
  for (let i = 0; i < name.length; i++) c = CRC_TABLE[(c ^ name.charCodeAt(i)) & 0xff]! ^ (c >>> 8);
  return c >>> 0;
}

export class NamesBin {
  private static readonly HEADER = 20;
  constructor(private buf: Buffer) {
    if (buf.length < NamesBin.HEADER) throw new Error('names.bin is too short');
  }
  get crc(): number {
    return this.buf.readUInt32LE(4);
  }
  at(offset: number): string {
    const start = NamesBin.HEADER + offset;
    if (start < NamesBin.HEADER || start >= this.buf.length) throw new Error(`names.bin offset ${offset} out of range`);
    const end = this.buf.indexOf(0, start);
    return this.buf.toString('latin1', start, end < 0 ? this.buf.length : end);
  }
}

export interface DefType {
  index: number;
  name: string;
}
export interface Def {
  index: number; // entry index
  group: string;
  name: string;
}
export interface Component {
  index: number; // entry index
  type: string;
}
export interface ComponentRef {
  type: string;
  typeHash: number;
  entry: number;
  owner: number;
}

const ROW = 12;
const NONE = 0xffffffff;

export class GameBin {
  readonly types: DefType[] = [];
  readonly defs: Def[] = [];
  readonly components: Component[] = [];
  private entries: Buffer[] = [];
  private byName = new Map<string, Def>();
  private typeByHash = new Map<number, string>();

  constructor(buf: Buffer, names: NamesBin) {
    if (buf[0] !== 0) throw new Error('game.bin does not use names.bin (Xbox build?)');
    const namesCrc = buf.readUInt32LE(5);
    if (namesCrc !== names.crc) throw new Error('game.bin and names.bin are from different builds');
    const count = buf.readUInt32LE(9);
    let o = 13;
    for (let i = 0; i < count; i++, o += ROW) {
      const a = buf.readUInt32LE(o);
      const b = buf.readUInt32LE(o + 4);
      const c = buf.readUInt32LE(o + 8);
      if (b === NONE) this.components.push({ index: i, type: names.at(a) });
      else if (c === 0 && this.defs.length === 0) this.types.push({ index: i, name: names.at(a) });
      else {
        const d = { index: i, group: names.at(a), name: names.at(b) };
        this.defs.push(d);
        this.byName.set(d.name, d);
      }
    }
    for (const t of this.types) this.typeByHash.set(fableHash(t.name), t.name);

    const chunks = buf.readUInt32LE(o);
    o += 4;
    const rows: [number, number][] = [];
    for (let i = 0; i < chunks; i++, o += 8) rows.push([buf.readUInt32LE(o), buf.readUInt32LE(o + 4)]);
    o += 4; // compressed size
    const base = o;
    for (let k = 0; k + 1 < rows.length; k++) {
      const [first, off] = rows[k]!;
      const [next, end] = rows[k + 1]!;
      const u = zlib.inflateSync(buf.subarray(base + off, base + end));
      const n = next - first;
      const starts: number[] = [];
      for (let j = 0; j < n; j++) starts.push(u.readUInt16LE(j * 2));
      starts.push(u.length);
      for (let j = 0; j < n; j++) this.entries.push(u.subarray(starts[j]!, starts[j + 1]!));
    }
    if (this.entries.length !== count) throw new Error(`game.bin: expected ${count} entries, decoded ${this.entries.length}`);
  }

  entry(index: number): Buffer {
    const e = this.entries[index];
    if (!e) throw new Error(`no entry ${index}`);
    return e;
  }

  def(name: string): Def | undefined {
    return this.byName.get(name);
  }

  /** Def name for a def reference found in a value, or undefined if it isn't a def. */
  defNameAt(entryIndex: number): string | undefined {
    const e = this.entries[entryIndex];
    if (!e) return undefined;
    const d = this.defs[entryIndex - this.types.length];
    return d && d.index === entryIndex ? d.name : undefined;
  }

  group(group: string): Def[] {
    return this.defs.filter((d) => d.group === group);
  }

  componentsOf(def: Def): ComponentRef[] {
    const b = this.entry(def.index);
    if (b.length < 5) return [];
    const n = b.readUInt16LE(3);
    const out: ComponentRef[] = [];
    for (let j = 0; j < n; j++) {
      const p = 5 + j * ROW;
      if (p + ROW > b.length) break;
      const typeHash = b.readUInt32LE(p);
      out.push({ type: this.typeByHash.get(typeHash) ?? `0x${typeHash.toString(16)}`, typeHash, entry: b.readUInt32LE(p + 4), owner: b.readUInt32LE(p + 8) });
    }
    return out;
  }

  component(def: Def, type: string): Buffer | undefined {
    const c = this.componentsOf(def).find((x) => x.type === type);
    return c && this.entry(c.entry);
  }

  /** The def's own fields (after its component table). */
  own(def: Def): Buffer {
    const b = this.entry(def.index);
    const n = b.length >= 5 ? b.readUInt16LE(3) : 0;
    return b.subarray(5 + n * ROW);
  }
}

// ---------------------------------------------------------------- field readers

/** Find a field by name. Field hashes are unique within an entry, so a byte search is safe. */
export function fieldOffset(entry: Buffer, field: string | number): number {
  const h = typeof field === 'number' ? field : fableHash(field);
  const key = Buffer.alloc(4);
  key.writeUInt32LE(h);
  const at = entry.indexOf(key);
  return at < 0 ? -1 : at + 4;
}

export function readF32(entry: Buffer | undefined, field: string | number): number | undefined {
  if (!entry) return undefined;
  const o = fieldOffset(entry, field);
  return o < 0 || o + 4 > entry.length ? undefined : round(entry.readFloatLE(o));
}

export function readI32(entry: Buffer | undefined, field: string | number): number | undefined {
  if (!entry) return undefined;
  const o = fieldOffset(entry, field);
  return o < 0 || o + 4 > entry.length ? undefined : entry.readInt32LE(o);
}

export function readU8(entry: Buffer | undefined, field: string | number): number | undefined {
  if (!entry) return undefined;
  const o = fieldOffset(entry, field);
  return o < 0 || o >= entry.length ? undefined : entry[o];
}

/** A per-level array: u32 count, then count values. Used by the spell defs (4 levels). */
export function readF32Array(entry: Buffer | undefined, field: string | number, max = 16): number[] | undefined {
  if (!entry) return undefined;
  const o = fieldOffset(entry, field);
  if (o < 0 || o + 4 > entry.length) return undefined;
  const n = entry.readUInt32LE(o);
  if (n === 0 || n > max || o + 4 + n * 4 > entry.length) return undefined;
  const out: number[] = [];
  for (let i = 0; i < n; i++) out.push(round(entry.readFloatLE(o + 4 + i * 4)));
  return out;
}

const round = (n: number) => Math.round(n * 10_000) / 10_000;
