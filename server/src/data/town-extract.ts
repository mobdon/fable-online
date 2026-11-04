import { GameBin, NamesBin, fableHash, fieldOffset, readI32, type Def } from './fable-bin.js';
import { levelScriptNames, tngBool, tngPlace, tngStr, tngThings } from './wad.js';
import { VILLAGE_ROLES, type PlacedVillage, type PlacedVillager, type Point3, type StockEntry, type TownData, type TownRegion, type VillagePeople } from '../world/town/town-data.js';

/**
 * Pulls towns out of the operator's install: what each kind of shop sells (CShopDef in
 * game.bin), who lives in each kind of village (CVillagePeopleDef), and where villages,
 * shops, homes and hand-placed villagers are (the .tng files in FinalAlbion.wad).
 *
 * CShopDef is a run of stock rows. Each row starts with `Type` (the OBJECT def), followed by
 * Quantity, PriceMultiplier, MaxStock and RestockPeriod. Those names hash to the field ids in
 * the file, so they are certain; see tools/re-notes/07-game-bin.md.
 */
export const TOWN_FIELDS = {
  shop: { type: 'Type', quantity: 'Quantity', priceMultiplier: 'PriceMultiplier', maxStock: 'MaxStock', restockPeriod: 'RestockPeriod' },
  people: { guard: 'Guard', numGuards: 'NumGuards' },
  enemy: { faction: 'Faction' },
} as const;

/** Stock rows are 64 bytes; a field belongs to a row when it sits before the next `Type`. */
const ROW_SPAN = 64;

/** Factions the town simulation runs. Monsters and bandits belong to SpawnManager. */
export const TOWN_FACTIONS = new Set(['FACTION_VILLAGERS', 'FACTION_TRADERS']);

export function extractShopTypes(g: GameBin): Record<string, StockEntry[]> {
  const out: Record<string, StockEntry[]> = {};
  const typeKey = Buffer.alloc(4);
  typeKey.writeUInt32LE(fableHash(TOWN_FIELDS.shop.type));
  for (const def of g.group('BUILDING')) {
    const b = g.component(def, 'CShopDef');
    if (!b) continue;
    const rows: StockEntry[] = [];
    let at = b.indexOf(typeKey);
    while (at >= 0 && at + 8 <= b.length) {
      const next = b.indexOf(typeKey, at + 4);
      const row = b.subarray(at, next < 0 ? Math.min(b.length, at + ROW_SPAN) : next);
      const item = g.defNameAt(row.readUInt32LE(4));
      if (item?.startsWith('OBJECT_')) {
        const f32 = (k: string) => {
          const o = fieldOffset(row, k);
          return o < 0 || o + 4 > row.length ? undefined : Math.round(row.readFloatLE(o) * 1000) / 1000;
        };
        // Quantity and MaxStock are floats; a row with both at 0 is a slot the shop leaves empty.
        const quantity = Math.max(0, Math.round(f32(TOWN_FIELDS.shop.quantity) ?? 0));
        const maxStock = Math.max(quantity, Math.round(f32(TOWN_FIELDS.shop.maxStock) ?? 0));
        if (maxStock > 0) rows.push({
          def: item,
          quantity,
          maxStock,
          priceMultiplier: f32(TOWN_FIELDS.shop.priceMultiplier) ?? 1,
          restockDays: Math.max(0, readI32(row, TOWN_FIELDS.shop.restockPeriod) ?? 0),
        });
      }
      at = next;
    }
    if (rows.length) out[def.name] = rows;
  }
  return out;
}

export function extractVillagePeople(g: GameBin): Record<string, VillagePeople> {
  const out: Record<string, VillagePeople> = {};
  const ref = (b: Buffer, field: string) => {
    const v = readI32(b, field);
    return v === undefined || v <= 0 ? undefined : g.defNameAt(v);
  };
  for (const def of g.group('VILLAGE')) {
    const b = g.component(def, 'CVillagePeopleDef');
    if (!b) continue;
    const roles: VillagePeople['roles'] = {};
    for (const r of VILLAGE_ROLES) {
      const c = ref(b, r);
      if (c?.startsWith('CREATURE_')) roles[r] = c;
    }
    const guard = ref(b, TOWN_FIELDS.people.guard);
    const n = readI32(b, TOWN_FIELDS.people.numGuards);
    out[def.name] = { roles, guard: guard?.startsWith('CREATURE_') ? guard : null, ...(n !== undefined ? { numGuards: n > 0 && n < 50 ? n : 0 } : {}) };
  }
  return out;
}

export function creatureFactions(g: GameBin): Map<string, string> {
  const out = new Map<string, string>();
  for (const def of g.group('CREATURE')) {
    const enemy = g.component(def as Def, 'CEnemyDef');
    const v = readI32(enemy, TOWN_FIELDS.enemy.faction);
    const f = v === undefined || v < 0 ? undefined : g.defNameAt(v);
    if (f) out.set(def.name, f);
  }
  return out;
}

/** Margin around a village's things that still counts as its ground (metres). */
export const ZONE_MARGIN = 8;
/** A village whose things are all in one spot still gets this much ground around it. */
export const ZONE_MIN_RADIUS = 25;

/**
 * Parse one level's .tng text. `faction` tells villagers from monsters.
 *
 * Guards: Fable's village system spawns a village's guards (CVillagePeopleDef NumGuards) and
 * walks them along its guard tracks, chains of TRACK_NODE_BASIC things named GuardTrack
 * (LinkedToUID2 is the next node; the first node is tied to the village). The guards placed
 * in the .tng are all quest people (each has a ScriptName), so they stay with each player's
 * own Fable like other quest people.
 */
export function parseTownTng(text: string, faction: (def: string) => string | undefined): TownRegion {
  const r: TownRegion = { villages: [], shops: [], homes: [], villagers: [] };
  const members = new Map<string, { x: number; y: number }[]>();
  const tracks = new Map<string, { p: Point3; next: string | null; start: boolean; village: string | null }>();
  for (const { kind, body } of tngThings(text)) {
    const def = tngStr(body, 'DefinitionType');
    const uid = tngStr(body, 'UID');
    if (!def || !uid) continue;
    const p = tngPlace(body);
    const village = body.includes('StartCTCVillageMember') ? (tngStr(body, 'VillageUID') ?? null) : null;
    if (village && village !== '0') {
      const m = members.get(village) ?? [];
      m.push({ x: p.x, y: p.y });
      members.set(village, m);
    }
    if (kind === 'TrackNode') {
      if (tngStr(body, 'ScriptName') !== 'GuardTrack') continue;
      const next = tngStr(body, 'LinkedToUID2');
      tracks.set(uid, { p: { x: r3(p.x), y: r3(p.y), z: r3(p.z) }, next: next && next !== '0' ? next : null, start: tngBool(body, 'Start') ?? false, village: village && village !== '0' ? village : null });
      continue;
    }
    if (kind === 'Village') r.villages.push({ uid, def, x: p.x, y: p.y, z: p.z });
    else if (kind === 'Building' && body.includes('StartCTCShop')) r.shops.push({ uid, building: def, village, ...p });
    else if (kind === 'Building' && tngBool(body, 'IsResidential')) r.homes.push({ uid, building: def, village, x: p.x, y: p.y, z: p.z });
    else if (kind === 'AICreature') {
      const script = tngStr(body, 'ScriptName');
      // Quest people (anything with a script name) stay with each player's own Fable.
      if (script && script !== 'NULL') continue;
      if (/GUARD/.test(def)) continue; // guards are the guard and crime system's
      if (!TOWN_FACTIONS.has(faction(def) ?? '')) continue;
      const v: PlacedVillager = { uid, def, village, ...p };
      r.villagers.push(v);
    }
  }
  const routes = guardRoutes(tracks);
  for (const v of r.villages) {
    const own = routes.filter((t) => t.village === v.uid).map((t) => t.points);
    if (own.length) v.guardRoutes = own;
    const pts = [{ x: v.x, y: v.y }, ...(members.get(v.uid) ?? []), ...own.flat()];
    v.zone = zoneAround(pts, { x: v.x, y: v.y });
  }
  return r;
}

/** Follow each GuardTrack chain from its first node. A chain belongs to the village any of its nodes is tied to. */
function guardRoutes(tracks: Map<string, { p: Point3; next: string | null; start: boolean; village: string | null }>) {
  const linked = new Set<string>();
  for (const t of tracks.values()) if (t.next) linked.add(t.next);
  const out: { village: string | null; points: Point3[] }[] = [];
  for (const [uid, t] of tracks) {
    if (linked.has(uid) && !t.start) continue; // not the head of a chain
    const points: Point3[] = [];
    let village: string | null = null;
    const seen = new Set<string>();
    for (let at: string | null = uid; at && !seen.has(at); ) {
      seen.add(at);
      const n = tracks.get(at);
      if (!n) break;
      points.push(n.p);
      village ??= n.village;
      at = n.next;
    }
    if (points.length >= 2 && village) out.push({ village, points });
  }
  return out;
}

/** Convex outline (counter-clockwise) of the points, pushed out by ZONE_MARGIN. */
export function zoneAround(points: { x: number; y: number }[], centre: { x: number; y: number }): { x: number; y: number }[] {
  const pts = [...points];
  // A ring keeps tiny villages (one building) from getting a sliver of a zone.
  for (let i = 0; i < 8; i++) {
    const a = (i / 8) * Math.PI * 2;
    pts.push({ x: centre.x + Math.cos(a) * ZONE_MIN_RADIUS, y: centre.y + Math.sin(a) * ZONE_MIN_RADIUS });
  }
  const hull = convexHull(pts);
  // Push each corner out from the hull's middle by the margin.
  const cx = hull.reduce((s, p) => s + p.x, 0) / hull.length;
  const cy = hull.reduce((s, p) => s + p.y, 0) / hull.length;
  return hull.map((p) => {
    const d = Math.hypot(p.x - cx, p.y - cy) || 1;
    return { x: r3(p.x + ((p.x - cx) / d) * ZONE_MARGIN), y: r3(p.y + ((p.y - cy) / d) * ZONE_MARGIN) };
  });
}

function convexHull(points: { x: number; y: number }[]): { x: number; y: number }[] {
  const p = [...points].sort((a, b) => a.x - b.x || a.y - b.y);
  if (p.length < 3) return p;
  const cross = (o: { x: number; y: number }, a: { x: number; y: number }, b: { x: number; y: number }) => (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  const lower: { x: number; y: number }[] = [];
  for (const q of p) {
    while (lower.length >= 2 && cross(lower[lower.length - 2]!, lower[lower.length - 1]!, q) <= 0) lower.pop();
    lower.push(q);
  }
  const upper: { x: number; y: number }[] = [];
  for (const q of p.reverse()) {
    while (upper.length >= 2 && cross(upper[upper.length - 2]!, upper[upper.length - 1]!, q) <= 0) upper.pop();
    upper.push(q);
  }
  return [...lower.slice(0, -1), ...upper.slice(0, -1)];
}

const r3 = (n: number) => Math.round(n * 1000) / 1000;

/** Where a level sits in its region: the region graph's name and the offset into the region's frame (tools/extractor/things.mjs regionFrames). */
export type LevelFrames = Record<string, { region: string; dx: number; dy: number }>;

/** Move a level's town into its region's frame. */
export function shiftTown(r: TownRegion, dx: number, dy: number): TownRegion {
  if (!dx && !dy) return r;
  const m = <T extends { x: number; y: number }>(p: T): T => ({ ...p, x: r3(p.x + dx), y: r3(p.y + dy) });
  return {
    villages: r.villages.map((v): PlacedVillage => ({
      ...m(v),
      ...(v.zone ? { zone: v.zone.map(m) } : {}),
      ...(v.guardRoutes ? { guardRoutes: v.guardRoutes.map((route) => route.map(m)) } : {}),
    })),
    shops: r.shops.map(m),
    homes: r.homes.map(m),
    villagers: r.villagers.map(m),
  };
}

/**
 * `frames` (from tools/extractor/things.mjs regionFrames) keys towns by the region graph's
 * names, which the server's channels use, in each region's frame. Without it they are keyed
 * by level script in level-local positions (old behaviour; towns in levels named unlike
 * their region, such as KnotholeGladeMainTown in KnotholeGlade, then never load).
 */
export function extractTown(gameBin: Buffer, namesBin: Buffer, tng: Map<string, string>, wldText: string, frames?: LevelFrames): TownData {
  const g = new GameBin(gameBin, new NamesBin(namesBin));
  const factions = creatureFactions(g);
  const scripts = levelScriptNames(wldText);
  const regions: Record<string, TownRegion> = {};
  for (const [base, text] of tng) {
    const script = scripts.get(base.toLowerCase()) ?? base;
    const f = frames?.[script];
    const region = f?.region ?? script;
    const r = shiftTown(parseTownTng(text, (d) => factions.get(d)), f?.dx ?? 0, f?.dy ?? 0);
    if (!r.villages.length && !r.shops.length && !r.villagers.length) continue;
    const into = (regions[region] ??= { villages: [], shops: [], homes: [], villagers: [] });
    into.villages.push(...r.villages);
    into.shops.push(...r.shops);
    into.homes.push(...r.homes);
    into.villagers.push(...r.villagers);
  }
  return {
    source: { kind: 'install', extractedAt: new Date().toISOString() },
    shopTypes: extractShopTypes(g),
    villages: extractVillagePeople(g),
    regions,
  };
}
