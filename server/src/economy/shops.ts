import fs from 'node:fs';
import { GameError } from '../errors.js';
import type { Vec3 } from '../world/sim.js';
import type { Economy, TxResult } from './economy.js';

/**
 * Shops: which vendor sells what, and where. Prices always come from the ItemCatalog
 * (Fable's base value); a shop only decides its stock. When a shop has a position the
 * hero must be standing near it, using the server's own copy of the hero's position.
 */
export interface ShopDef {
  id: string;
  region: string;
  pos?: Vec3;
  stock: string[];
}

export const SHOP_REACH = 6;

export class Shops {
  private shops = new Map<string, ShopDef>();

  constructor(
    private economy: Economy,
    defs: ShopDef[] = [],
  ) {
    for (const d of defs) {
      for (const item of d.stock) {
        const def = economy.catalog.get(item); // throws for an unknown item: a bad shop file fails at boot
        if (def.buy === null) throw new Error(`shop ${d.id} stocks ${item}, which vendors never sell`);
      }
      this.shops.set(d.id, d);
    }
  }

  static load(economy: Economy, file: string | undefined): Shops {
    if (!file || !fs.existsSync(file)) return new Shops(economy);
    const raw = JSON.parse(fs.readFileSync(file, 'utf8')) as { shops: ShopDef[] };
    return new Shops(economy, raw.shops ?? []);
  }

  get(id: string): ShopDef | undefined {
    return this.shops.get(id);
  }

  buy(heroId: number, shopId: string, defId: string, quantity: number, key: string, at?: { region: string; pos: Vec3 }): TxResult & { itemId: string } {
    const s = this.check(shopId, at);
    if (!s.stock.includes(defId)) throw new GameError('not_stocked', 'This shop does not sell that');
    return this.economy.vendorBuy(heroId, defId, quantity, key);
  }

  sell(heroId: number, shopId: string, itemId: string, key: string, at?: { region: string; pos: Vec3 }): TxResult & { gold: number } {
    this.check(shopId, at);
    return this.economy.vendorSell(heroId, itemId, key);
  }

  private check(shopId: string, at?: { region: string; pos: Vec3 }): ShopDef {
    const s = this.shops.get(shopId);
    if (!s) throw new GameError('no_shop', 'No such shop', 404);
    if (at) {
      if (at.region !== s.region) throw new GameError('too_far', 'You are not at that shop');
      if (s.pos && Math.hypot(at.pos.x - s.pos.x, at.pos.y - s.pos.y) > SHOP_REACH) throw new GameError('too_far', 'You are not at that shop');
    }
    return s;
  }
}
