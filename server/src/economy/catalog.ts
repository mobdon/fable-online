import { GameError } from '../errors.js';
import { BLOCKED } from '../world/things/loot.js';

/**
 * Server-side item rules. Prices live ONLY here: a client never sends a price.
 * The real table will be generated from game.bin by the extractor; this is a starter set.
 */
export interface ItemDef {
  id: string;
  maxStack: number; // 1 = not stackable
  /** Price a vendor charges. null = vendors never sell it (e.g. Resurrection Phials). */
  buy: number | null;
  /** Gold a vendor pays. null = vendors won't buy it. */
  sell: number | null;
  /** Can be traded between players. */
  tradeable: boolean;
  /** Staff gear: only a GM action can create it, and it can never be sold, dropped or traded. */
  staffOnly?: boolean;
}

/** A vendor never pays more than this share of its own price, so buy-then-sell always loses gold. */
export const MAX_SELL_RATIO = 0.5;

export const STARTER_ITEMS: ItemDef[] = [
  { id: 'HealthPotion', maxStack: 20, buy: 100, sell: 25, tradeable: true },
  { id: 'WillPotion', maxStack: 20, buy: 120, sell: 30, tradeable: true },
  { id: 'IronLongsword', maxStack: 1, buy: 800, sell: 200, tradeable: true },
  { id: 'Apple', maxStack: 50, buy: 5, sell: 1, tradeable: true },
  { id: 'ResurrectionPhial', maxStack: 1, buy: null, sell: null, tradeable: false },
  { id: 'GuildSeal', maxStack: 1, buy: null, sell: null, tradeable: false },
];

/** Vendors pay this share of an item's base value (armour balance design: flat 25%). */
export const SELL_BACK = 0.25;

/** Items that never come from shops or loot, whatever the game files say: any Resurrection Phial def. */
export const UNSHOPPABLE = /RESURRECTION_PHIAL/;

/**
 * Armour classes no vendor sells. Plate comes from drops and the Arena only; vendors still
 * buy it back. (Platinum is staff gear and is closed separately.)
 */
export const NOT_SOLD_ARMOUR: ReadonlySet<string> = new Set(['ARMOUR_HERO_PLATE_ARMOUR']);

/**
 * Turn the extracted item table into catalog entries. Fable's base value is the shop
 * price; vendors buy back at SELL_BACK. Resurrection Phials and staff gear are never sold
 * or bought; plate armour (NOT_SOLD_ARMOUR) is never sold but can be sold back.
 */
export function catalogFromGameData(
  items: Record<string, { value: number; weapon?: unknown; coverage?: number; armour?: string }>,
  staffArmour: string,
  notSoldArmour: ReadonlySet<string> = NOT_SOLD_ARMOUR,
): ItemDef[] {
  const out: ItemDef[] = [];
  for (const [id, it] of Object.entries(items)) {
    // By name as well as by armour reference, so platinum stays staff only if the extracted
    // reference is missing or renamed.
    const staffOnly = it.armour === staffArmour || /PLATINUM/.test(it.armour ?? '') || BLOCKED.some((r) => r.test(id));
    const gear = !!it.weapon || it.coverage !== undefined;
    const value = Math.max(0, Math.round(it.value));
    const closed = staffOnly || UNSHOPPABLE.test(id) || value === 0;
    const notSold = closed || (it.armour !== undefined && notSoldArmour.has(it.armour));
    out.push({
      id,
      maxStack: gear ? 1 : /POTION|FOOD|APPLE|BEER|MEAT|FISH|ARROW/.test(id) ? 20 : 1,
      buy: notSold ? null : value,
      sell: closed ? null : Math.floor(value * SELL_BACK),
      tradeable: !staffOnly && !UNSHOPPABLE.test(id),
      ...(staffOnly ? { staffOnly } : {}),
    });
  }
  return out;
}

export class ItemCatalog {
  private defs = new Map<string, ItemDef>();

  constructor(defs: ItemDef[]) {
    for (const d of defs) this.add(d);
  }

  /** Register one def. Same validation as the constructor. */
  add(d: ItemDef): void {
    if (!Number.isInteger(d.maxStack) || d.maxStack < 1) throw new Error(`${d.id}: bad maxStack`);
    for (const p of [d.buy, d.sell]) if (p !== null && (!Number.isInteger(p) || p < 0)) throw new Error(`${d.id}: bad price`);
    // The loophole check: if you could buy for X and sell for >= X you could print gold.
    if (d.buy !== null && d.sell !== null && d.sell > Math.floor(d.buy * MAX_SELL_RATIO))
      throw new Error(`${d.id}: sell price ${d.sell} must be <= ${MAX_SELL_RATIO} x buy price ${d.buy}`);
    if (this.defs.has(d.id)) throw new Error(`duplicate item def ${d.id}`);
    this.defs.set(d.id, d);
  }

  has(id: string): boolean {
    return this.defs.has(id);
  }

  get(id: string): ItemDef {
    const d = this.defs.get(id);
    if (!d) throw new GameError('bad_item', `Unknown item ${id}`);
    return d;
  }
}
