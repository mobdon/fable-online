import type { ItemCatalog } from '../../economy/catalog.js';
import type { ShopDef } from '../../economy/shops.js';
import type { TownData } from './town-data.js';
import { shopIdOf } from './townsfolk.js';

/** Stalls and wagons are small; a shop building's origin is inside, so the counter is further away. */
const STALL_REACH = 6;
const BUILDING_REACH = 14;

/**
 * One server shop per Fable shop building, stocked from that building's CShopDef. Items the
 * catalog doesn't know, or that vendors never sell (Resurrection Phials, plate armour, staff gear), are left
 * out rather than failing the boot.
 */
export function shopsFromTown(town: TownData, catalog: ItemCatalog): ShopDef[] {
  const out: ShopDef[] = [];
  for (const [region, r] of Object.entries(town.regions)) {
    for (const s of r.shops) {
      const lines = (town.shopTypes[s.building] ?? []).filter((l) => catalog.has(l.def) && catalog.get(l.def).buy !== null);
      const stock = [...new Set(lines.map((l) => l.def))];
      if (!stock.length) continue;
      out.push({
        id: shopIdOf(region, s.uid),
        region,
        pos: { x: s.x, y: s.y, z: s.z },
        reach: /STALL|WAGON|TENT/.test(s.building) ? STALL_REACH : BUILDING_REACH,
        stock,
        lines: lines.map(({ def, maxStock, priceMultiplier, restockDays }) => ({ def, maxStock, priceMultiplier, restockDays })),
        building: s.building,
      });
    }
  }
  return out;
}
