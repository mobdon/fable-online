import crypto from 'node:crypto';
import { GameError } from '../errors.js';
import type { Economy, TradeSide } from './economy.js';

/**
 * Player-to-player trade window. Classic scam guard: any change to either offer clears
 * both "accept" ticks, and the trade executes against the exact offers both sides accepted.
 */
interface Trade {
  id: string;
  sides: [TradeSide, TradeSide];
  accepted: [boolean, boolean];
  version: number;
  expiresAt: number;
}

const TRADE_TTL_MS = 5 * 60_000;
const MAX_ITEMS_PER_SIDE = 16;

export class TradeSessions {
  private trades = new Map<string, Trade>();
  private byHero = new Map<number, string>();

  constructor(private economy: Economy) {}

  open(heroA: number, heroB: number): string {
    if (heroA === heroB) throw new GameError('bad_trade', 'Cannot trade with yourself');
    for (const h of [heroA, heroB]) if (this.byHero.has(h)) throw new GameError('busy', 'Already trading');
    const id = crypto.randomUUID();
    this.trades.set(id, {
      id,
      sides: [
        { heroId: heroA, gold: 0, itemIds: [] },
        { heroId: heroB, gold: 0, itemIds: [] },
      ],
      accepted: [false, false],
      version: 0,
      expiresAt: Date.now() + TRADE_TTL_MS,
    });
    this.byHero.set(heroA, id);
    this.byHero.set(heroB, id);
    return id;
  }

  setOffer(heroId: number, gold: number, itemIds: string[]): number {
    const [t, i] = this.find(heroId);
    if (!Number.isSafeInteger(gold) || gold < 0) throw new GameError('bad_amount', 'Bad gold amount');
    if (!Array.isArray(itemIds) || itemIds.length > MAX_ITEMS_PER_SIDE) throw new GameError('bad_trade', 'Too many items');
    if (gold > this.economy.balance(heroId)) throw new GameError('insufficient_gold', 'Not enough gold');
    for (const id of itemIds) {
      const it = this.economy.item(id);
      if (!it || it.owner_hero !== heroId || it.location !== 'inventory') throw new GameError('not_owner', 'You do not have that item');
      if (!this.economy.catalog.get(it.def_id).tradeable) throw new GameError('untradeable', `${it.def_id} cannot be traded`);
    }
    t.sides[i] = { heroId, gold, itemIds: [...itemIds] };
    t.accepted = [false, false];
    return ++t.version;
  }

  /**
   * Accept the offer at `version`. When both sides have accepted the same version, the
   * trade executes atomically. Returns the ledger tx id once executed.
   */
  accept(heroId: number, version: number): { done: boolean; txId?: number } {
    const [t, i] = this.find(heroId);
    if (version !== t.version) throw new GameError('offer_changed', 'The offer changed, check it again');
    t.accepted[i] = true;
    if (!t.accepted[0] || !t.accepted[1]) return { done: false };
    this.close(t);
    const r = this.economy.executeTrade(t.sides[0], t.sides[1], `trade:${t.id}`);
    return { done: true, txId: r.txId };
  }

  cancel(heroId: number): void {
    const id = this.byHero.get(heroId);
    const t = id && this.trades.get(id);
    if (t) this.close(t);
  }

  sweep(now = Date.now()): void {
    for (const t of this.trades.values()) if (t.expiresAt < now) this.close(t);
  }

  private find(heroId: number): [Trade, 0 | 1] {
    const id = this.byHero.get(heroId);
    const t = id ? this.trades.get(id) : undefined;
    if (!t || t.expiresAt < Date.now()) throw new GameError('no_trade', 'No open trade');
    return [t, t.sides[0].heroId === heroId ? 0 : 1];
  }

  private close(t: Trade): void {
    this.trades.delete(t.id);
    for (const s of t.sides) this.byHero.delete(s.heroId);
  }
}
