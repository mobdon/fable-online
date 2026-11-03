import fs from 'node:fs';

/**
 * Towns: villages, the shops and homes in them, and who lives there. Generated from the
 * operator's own install by `npm run extract-town -- "<Fable folder>"` (src/data/town-extract.ts)
 * into server/data/town-data.json, never committed. Without it the server uses FALLBACK_TOWN,
 * a small made-up village for tests and dev.
 *
 * Positions are level-local metres, z up, the same frame as spawns.json and the navmesh.
 */

/** One line of a shop's stock (Fable's CShopDef rows). */
export interface StockEntry {
  /** OBJECT_* def. */
  def: string;
  /** How many the shop has when it opens (Quantity). */
  quantity: number;
  /** It restocks up to this (MaxStock). */
  maxStock: number;
  /** Fable's markup on the item's base value (PriceMultiplier). */
  priceMultiplier: number;
  /** Game days between restocks (RestockPeriod). */
  restockDays: number;
}

/** The roles a CVillagePeopleDef fills, by the field names Fable uses. */
export const VILLAGE_ROLES = ['MaleShopKeeper', 'MaleBarman', 'FemaleBarmaid', 'MaleWarehouseWorker', 'MaleUnemployed', 'FemaleUnemployed'] as const;
export type VillageRole = (typeof VILLAGE_ROLES)[number];

export interface VillagePeople {
  /** Creature def per role; a role the village doesn't have is missing. */
  roles: Partial<Record<VillageRole, string>>;
  guard: string | null;
}

export interface PlacedVillage {
  uid: string;
  def: string;
  x: number;
  y: number;
  z: number;
}

export interface PlacedShop {
  /** The shop building's UID in the .tng. The client reports this when its Fable opens the shop. */
  uid: string;
  /** BUILDING_* def; its CShopDef is the stock list. */
  building: string;
  village: string | null;
  x: number;
  y: number;
  z: number;
  yaw: number;
}

export interface PlacedHome {
  uid: string;
  building: string;
  village: string | null;
  x: number;
  y: number;
  z: number;
}

/** A villager or trader the level designer placed by hand, not run by a quest script. */
export interface PlacedVillager {
  uid: string;
  def: string;
  village: string | null;
  x: number;
  y: number;
  z: number;
  yaw: number;
}

export interface TownRegion {
  villages: PlacedVillage[];
  shops: PlacedShop[];
  homes: PlacedHome[];
  villagers: PlacedVillager[];
}

export interface TownData {
  source: { kind: 'install' | 'fallback'; extractedAt?: string };
  /** BUILDING_* def -> what that kind of shop sells. */
  shopTypes: Record<string, StockEntry[]>;
  /** VILLAGE_* def -> who lives there. */
  villages: Record<string, VillagePeople>;
  /** Level script name -> what is placed there. */
  regions: Record<string, TownRegion>;
}

const stock = (def: string, n = 3, mult = 1.1): StockEntry => ({ def, quantity: n, maxStock: n, priceMultiplier: mult, restockDays: 3 });

/** A made-up village in the shape of the real data. Not taken from the game files. */
export const FALLBACK_TOWN: TownData = {
  source: { kind: 'fallback' },
  shopTypes: {
    BUILDING_TEST_GENERAL_STORE: [stock('OBJECT_HEALTH_POTION', 5), stock('OBJECT_WILL_POTION', 3)],
    BUILDING_TEST_SMITHY: [stock('OBJECT_IRON_LONGSWORD', 2, 1.2), stock('OBJECT_IRON_WARHAMMER', 1, 1.2), stock('OBJECT_YEW_LONGBOW', 1)],
  },
  villages: {
    VILLAGE_TEST: {
      roles: { MaleShopKeeper: 'CREATURE_TEST_SHOPKEEPER', MaleUnemployed: 'CREATURE_TEST_VILLAGER_MALE', FemaleUnemployed: 'CREATURE_TEST_VILLAGER_FEMALE' },
      guard: 'CREATURE_TEST_GUARD',
    },
  },
  regions: {
    TestRegion: {
      villages: [{ uid: 'v1', def: 'VILLAGE_TEST', x: 50, y: 50, z: 0 }],
      shops: [
        { uid: 's1', building: 'BUILDING_TEST_GENERAL_STORE', village: 'v1', x: 40, y: 50, z: 0, yaw: 0 },
        { uid: 's2', building: 'BUILDING_TEST_SMITHY', village: 'v1', x: 60, y: 50, z: 0, yaw: 0 },
      ],
      homes: [
        { uid: 'h1', building: 'BUILDING_TEST_HOUSE', village: 'v1', x: 45, y: 65, z: 0 },
        { uid: 'h2', building: 'BUILDING_TEST_HOUSE', village: 'v1', x: 55, y: 65, z: 0 },
        { uid: 'h3', building: 'BUILDING_TEST_HOUSE', village: 'v1', x: 50, y: 35, z: 0 },
      ],
      villagers: [{ uid: 'p1', def: 'CREATURE_TEST_TRADER', village: null, x: 80, y: 50, z: 0, yaw: 0 }],
    },
  },
};

export function loadTownData(file: string | undefined): TownData {
  if (!file || !fs.existsSync(file)) return FALLBACK_TOWN;
  const d = JSON.parse(fs.readFileSync(file, 'utf8')) as TownData;
  if (!d.regions || !d.shopTypes) throw new Error(`${file} is not a town-data file`);
  return { ...d, villages: d.villages ?? {} };
}
