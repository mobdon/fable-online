import { GameBin, NamesBin, fableHash, fieldOffset, readI32, type Def } from './fable-bin.js';
import { levelScriptNames, tngBool, tngPlace, tngStr, tngThings } from './wad.js';
import { VILLAGE_ROLES, type PlacedVillager, type StockEntry, type TownData, type TownRegion, type VillagePeople } from '../world/town/town-data.js';

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
  people: { guard: 'Guard' },
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
    out[def.name] = { roles, guard: guard?.startsWith('CREATURE_') ? guard : null };
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

/** Parse one region's .tng text. `faction` tells villagers from monsters. */
export function parseTownTng(text: string, faction: (def: string) => string | undefined): TownRegion {
  const r: TownRegion = { villages: [], shops: [], homes: [], villagers: [] };
  for (const { kind, body } of tngThings(text)) {
    const def = tngStr(body, 'DefinitionType');
    const uid = tngStr(body, 'UID');
    if (!def || !uid) continue;
    const p = tngPlace(body);
    const village = body.includes('StartCTCVillageMember') ? (tngStr(body, 'VillageUID') ?? null) : null;
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
  return r;
}

export function extractTown(gameBin: Buffer, namesBin: Buffer, tng: Map<string, string>, wldText: string): TownData {
  const g = new GameBin(gameBin, new NamesBin(namesBin));
  const factions = creatureFactions(g);
  const scripts = levelScriptNames(wldText);
  const regions: Record<string, TownRegion> = {};
  for (const [base, text] of tng) {
    const region = scripts.get(base.toLowerCase()) ?? base;
    const r = parseTownTng(text, (d) => factions.get(d));
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
