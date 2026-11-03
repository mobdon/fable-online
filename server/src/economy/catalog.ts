import { GameError } from '../errors.js';

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

export class ItemCatalog {
  private defs = new Map<string, ItemDef>();

  constructor(defs: ItemDef[]) {
    for (const d of defs) {
      if (!Number.isInteger(d.maxStack) || d.maxStack < 1) throw new Error(`${d.id}: bad maxStack`);
      for (const p of [d.buy, d.sell]) if (p !== null && (!Number.isInteger(p) || p < 0)) throw new Error(`${d.id}: bad price`);
      // The loophole check: if you could buy for X and sell for >= X you could print gold.
      if (d.buy !== null && d.sell !== null && d.sell > Math.floor(d.buy * MAX_SELL_RATIO))
        throw new Error(`${d.id}: sell price ${d.sell} must be <= ${MAX_SELL_RATIO} x buy price ${d.buy}`);
      if (this.defs.has(d.id)) throw new Error(`duplicate item def ${d.id}`);
      this.defs.set(d.id, d);
    }
  }

  get(id: string): ItemDef {
    const d = this.defs.get(id);
    if (!d) throw new GameError('bad_item', `Unknown item ${id}`);
    return d;
  }
}
