import fs from 'node:fs';
import { GameError } from '../errors.js';
import type { Vec3 } from '../world/sim.js';
import type { Economy, TxResult } from './economy.js';

/**
 * Shops: which vendor sells what, and where. Prices always come from the ItemCatalog
 * (Fable's base value); a shop only decides its stock. When a shop has a position the
 * hero must be standing near it, using the server's own copy of the hero's position.
 *
 * Shops built from Fable's own shop defs (town data) also carry per-item stock lines: how
 * many the shop holds, how fast it restocks, and Fable's markup. Stock is shared by every
 * channel of the realm, so hopping channels doesn't refill a shop.
 */
export interface StockLine {
  def: string;
  maxStock: number;
  /** Fable's markup. Clamped to at least 1 so a shop never sells below the item's base value. */
  priceMultiplier: number;
  /** Game days between restocks; 0 = never runs out. */
  restockDays: number;
}

export interface ShopDef {
  id: string;
  region: string;
  pos?: Vec3;
  stock: string[];
  /** How close the hero must stand. Default SHOP_REACH; building shops are bigger than stalls. */
  reach?: number;
  /** Per-item limits and markup. An item in `stock` without a line has unlimited stock at base price. */
  lines?: StockLine[];
  /** BUILDING_* def, for logs and the client. */
  building?: string;
}

export interface ShopListing {
  def: string;
  price: number;
  /** null = unlimited. */
  left: number | null;
}

export const SHOP_REACH = 6;
/** Highest markup accepted from the game files, so a corrupt def can't price an apple at 10,000. */
export const MAX_MARKUP = 3;

interface Level {
  left: number;
  /** Game day of the last restock. */
  day: number;
}

export interface ShopOptions {
  /** Current game day (WorldClock), for restocks. Without it stock never refills. */
  day?: () => number;
}

export class Shops {
  private shops = new Map<string, ShopDef>();
  private lines = new Map<string, Map<string, StockLine>>();
  private levels = new Map<string, Map<string, Level>>();

  constructor(
    private economy: Economy,
    defs: ShopDef[] = [],
    private opts: ShopOptions = {},
  ) {
    for (const d of defs) {
      for (const item of d.stock) {
        const def = economy.catalog.get(item); // throws for an unknown item: a bad shop file fails at boot
        if (def.buy === null) throw new Error(`shop ${d.id} stocks ${item}, which vendors never sell`);
      }
      if (this.shops.has(d.id)) throw new Error(`duplicate shop ${d.id}`);
      this.shops.set(d.id, d);
      const lines = new Map<string, StockLine>();
      for (const l of d.lines ?? []) if (d.stock.includes(l.def)) lines.set(l.def, { ...l, priceMultiplier: clampMarkup(l.priceMultiplier) });
      this.lines.set(d.id, lines);
    }
  }

  static load(economy: Economy, file: string | undefined, extra: ShopDef[] = [], opts: ShopOptions = {}): Shops {
    const fromFile = file && fs.existsSync(file) ? ((JSON.parse(fs.readFileSync(file, 'utf8')) as { shops: ShopDef[] }).shops ?? []) : [];
    const ids = new Set(fromFile.map((s) => s.id));
    // A hand-written shop in content/shops.json overrides the generated one with the same id.
    return new Shops(economy, [...fromFile, ...extra.filter((s) => !ids.has(s.id))], opts);
  }

  get(id: string): ShopDef | undefined {
    return this.shops.get(id);
  }

  get size(): number {
    return this.shops.size;
  }

  /** What the shop sells right now, at what price, and how many are left. */
  listing(shopId: string): ShopListing[] {
    const s = this.shops.get(shopId);
    if (!s) throw new GameError('no_shop', 'No such shop', 404);
    return s.stock.map((def) => ({ def, price: this.price(shopId, def), left: this.left(shopId, def) }));
  }

  price(shopId: string, defId: string): number {
    const base = this.economy.catalog.get(defId).buy;
    if (base === null) throw new GameError('not_for_sale', 'Vendors do not sell that');
    const m = this.lines.get(shopId)?.get(defId)?.priceMultiplier ?? 1;
    return Math.ceil(base * m);
  }

  /** What the shop pays for an item (the catalog's sell-back price; null = won't buy it). */
  offer(defId: string): number | null {
    return this.economy.catalog.has(defId) ? this.economy.catalog.get(defId).sell : null;
  }

  buy(heroId: number, shopId: string, defId: string, quantity: number, key: string, at?: { region: string; pos: Vec3 }): TxResult & { itemId: string; price: number } {
    const s = this.check(shopId, at);
    if (!s.stock.includes(defId)) throw new GameError('not_stocked', 'This shop does not sell that');
    const left = this.left(shopId, defId);
    if (left !== null && left < quantity) throw new GameError('out_of_stock', left === 0 ? 'Sold out' : `Only ${left} left`);
    const price = this.price(shopId, defId);
    const r = this.economy.vendorBuy(heroId, defId, quantity, key, price);
    // A replayed request was already counted the first time.
    if (!r.replayed && left !== null) this.levels.get(shopId)!.get(defId)!.left -= quantity;
    return { ...r, price };
  }

  sell(heroId: number, shopId: string, itemId: string, key: string, at?: { region: string; pos: Vec3 }): TxResult & { gold: number } {
    this.check(shopId, at);
    return this.economy.vendorSell(heroId, itemId, key);
  }

  private left(shopId: string, defId: string): number | null {
    const line = this.lines.get(shopId)?.get(defId);
    if (!line || line.restockDays === 0) return null;
    const day = this.opts.day?.() ?? 0;
    let byDef = this.levels.get(shopId);
    if (!byDef) this.levels.set(shopId, (byDef = new Map()));
    let lv = byDef.get(defId);
    if (!lv) byDef.set(defId, (lv = { left: line.maxStock, day }));
    else if (day - lv.day >= line.restockDays) {
      lv.left = line.maxStock;
      lv.day = day;
    }
    return lv.left;
  }

  private check(shopId: string, at?: { region: string; pos: Vec3 }): ShopDef {
    const s = this.shops.get(shopId);
    if (!s) throw new GameError('no_shop', 'No such shop', 404);
    if (at) {
      if (at.region !== s.region) throw new GameError('too_far', 'You are not at that shop');
      if (s.pos && Math.hypot(at.pos.x - s.pos.x, at.pos.y - s.pos.y) > (s.reach ?? SHOP_REACH)) throw new GameError('too_far', 'You are not at that shop');
    }
    return s;
  }
}

function clampMarkup(m: number): number {
  return Number.isFinite(m) ? Math.min(MAX_MARKUP, Math.max(1, m)) : 1;
}
