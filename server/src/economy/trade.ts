import crypto from 'node:crypto';
import { GameError } from '../errors.js';
import { MAX_AMOUNT, type Economy, type TradeSide } from './economy.js';

/**
 * Player-to-player trade window, server side. The flow:
 *
 *   1. A asks B (`request`). B gets an invite and answers it (`answer`). Both must be in the
 *      same channel, within TRADE_REACH of each other, and allowed to trade (not in prison,
 *      not wanted). A hero has at most one window and one outgoing invite at a time.
 *   2. Each side sets its offer: gold plus up to MAX_ITEMS_PER_SIDE items, each the whole
 *      stack or part of it. Every offer is checked against the ledger when it is set.
 *   3. Each side accepts the offer *version* it saw. Any change to either offer bumps the
 *      version and clears both ticks, so a bait and switch can never be accepted. An accept
 *      is refused for SETTLE_MS after a change, so the other player has time to see it.
 *   4. When both sides have accepted the same version, Economy.executeTrade swaps everything
 *      in one DB transaction, keyed by trade id and version. It re-checks ownership, gold,
 *      stolen marks, staff gear and the law inside that transaction, so an item sold or
 *      equipped after it was offered fails the whole trade and nothing moves.
 *
 * Nothing is held in escrow while the window is open: the items stay in the owner's
 * inventory and the swap either happens whole or not at all. A failed swap keeps the window
 * open with both ticks cleared. Leaving the channel, walking away, logging out or 5 minutes
 * without a change closes it.
 */

export const TRADE_REACH = 10;
export const MAX_ITEMS_PER_SIDE = 16;
export const SETTLE_MS = 2_000;
const TRADE_TTL_MS = 5 * 60_000;
const INVITE_TTL_MS = 30_000;
const CHECK_EVERY_MS = 1_000;

export interface TradeItem {
  item: string;
  def: string;
  qty: number;
}

/** What the trade window needs from the world. Tests and the app each supply one. */
export interface TradeHost {
  send(heroId: number, msg: Record<string, unknown>): void;
  /** The hero's channel and position, or undefined when not in the world. */
  where(heroId: number): { channel: string; x: number; y: number; z: number } | undefined;
  name(heroId: number): string;
}

export type TradeCloseReason = 'cancelled' | 'left' | 'too_far' | 'expired' | 'blocked';

interface Offer {
  gold: number;
  items: { id: string; qty?: number }[];
}

interface Trade {
  id: string;
  heroes: [number, number];
  offers: [Offer, Offer];
  accepted: [boolean, boolean];
  version: number;
  changedAt: number;
  expiresAt: number;
}

interface Invite {
  from: number;
  to: number;
  expiresAt: number;
}

const NO_HOST: TradeHost = { send: () => {}, where: () => undefined, name: (id) => `Hero ${id}` };

export class TradeSessions {
  private trades = new Map<string, Trade>();
  private byHero = new Map<number, string>();
  /** Outgoing invites, by sender. */
  private invites = new Map<number, Invite>();
  private checkIn = CHECK_EVERY_MS;

  constructor(
    private economy: Economy,
    private host: TradeHost = NO_HOST,
    private clock: () => number = Date.now,
  ) {}

  attach(host: TradeHost): void {
    this.host = host;
  }

  isTrading(heroId: number): boolean {
    return this.byHero.has(heroId);
  }

  // ---------------------------------------------------------------- invites

  /** `from` asks `to` to trade. If `to` already asked `from`, the window opens at once. */
  request(from: number, to: number): void {
    if (from === to) throw new GameError('bad_trade', 'Cannot trade with yourself');
    this.checkPair(from, to);
    const back = this.invites.get(to);
    if (back && back.to === from && back.expiresAt >= this.clock()) {
      this.invites.delete(to);
      this.open(to, from);
      return;
    }
    const prior = this.invites.get(from);
    if (prior && prior.to !== to) this.host.send(prior.to, { t: 'trade_invite_gone', from });
    this.invites.set(from, { from, to, expiresAt: this.clock() + INVITE_TTL_MS });
    this.host.send(to, { t: 'trade_invite', from, name: this.host.name(from), expires_ms: INVITE_TTL_MS });
    this.host.send(from, { t: 'trade_asked', to, name: this.host.name(to) });
  }

  /** `to` answers the invite from `from`. */
  answer(to: number, from: number, yes: boolean): void {
    const inv = this.invites.get(from);
    if (!inv || inv.to !== to || inv.expiresAt < this.clock()) throw new GameError('no_invite', 'That invitation has gone');
    this.invites.delete(from);
    if (!yes) {
      this.host.send(from, { t: 'trade_declined', by: to, name: this.host.name(to) });
      return;
    }
    this.checkPair(from, to);
    this.open(from, to);
  }

  /** Open a window straight away. `request` and `answer` check the pair first. */
  open(heroA: number, heroB: number): string {
    if (heroA === heroB) throw new GameError('bad_trade', 'Cannot trade with yourself');
    for (const h of [heroA, heroB]) if (this.byHero.has(h)) throw new GameError('busy', 'Already trading');
    const now = this.clock();
    const id = crypto.randomUUID();
    const t: Trade = {
      id,
      heroes: [heroA, heroB],
      offers: [
        { gold: 0, items: [] },
        { gold: 0, items: [] },
      ],
      accepted: [false, false],
      version: 0,
      changedAt: now,
      expiresAt: now + TRADE_TTL_MS,
    };
    this.trades.set(id, t);
    for (const h of t.heroes) {
      this.byHero.set(h, id);
      this.invites.delete(h);
    }
    this.push(t, true);
    return id;
  }

  // ---------------------------------------------------------------- offers

  /**
   * Set this hero's whole offer. Returns the new version. `items` are item IDs from the
   * hero's carried inventory, each with an optional quantity for part of a stack.
   */
  setOffer(heroId: number, gold: number, items: (string | { id: string; qty?: number })[]): number {
    const [t, i] = this.find(heroId);
    if (!Number.isSafeInteger(gold) || gold < 0 || gold > MAX_AMOUNT) throw new GameError('bad_amount', 'Bad gold amount');
    if (!Array.isArray(items) || items.length > MAX_ITEMS_PER_SIDE) throw new GameError('bad_trade', 'Too many items');
    const offer: Offer = { gold, items: items.map((x) => (typeof x === 'string' ? { id: x } : { id: x.id, qty: x.qty })) };
    if (new Set(offer.items.map((x) => x.id)).size !== offer.items.length) throw new GameError('bad_trade', 'Duplicate item in offer');
    this.validate(heroId, offer);
    t.offers[i] = offer;
    t.accepted = [false, false];
    t.version++;
    t.changedAt = this.clock();
    t.expiresAt = t.changedAt + TRADE_TTL_MS;
    this.push(t);
    return t.version;
  }

  /**
   * Accept the offers at `version`. When both sides have accepted the same version the trade
   * executes atomically. A swap that fails keeps the window open with both ticks cleared,
   * tells both sides why (`trade_error`) and returns the error code.
   */
  accept(heroId: number, version: number): { done: boolean; txId?: number; error?: string } {
    const [t, i] = this.find(heroId);
    if (version !== t.version) throw new GameError('offer_changed', 'The offer changed, check it again');
    if (this.clock() - t.changedAt < SETTLE_MS) throw new GameError('too_soon', 'The offer just changed, check it again');
    // Re-check this hero's own side now, so a stale offer fails here and not at the swap.
    this.validate(heroId, t.offers[i]);
    t.accepted[i] = true;
    if (!t.accepted[0] || !t.accepted[1]) {
      this.push(t);
      return { done: false };
    }
    const [a, b] = [0, 1].map((k) => this.side(t, k as 0 | 1));
    try {
      this.checkPair(t.heroes[0], t.heroes[1], t.id);
      const r = this.economy.executeTrade(a!, b!, `trade:${t.id}:${t.version}`);
      this.drop(t);
      for (const h of t.heroes) this.host.send(h, { t: 'trade_done', trade: t.id, gold: this.economy.balance(h) });
      return { done: true, txId: r.txId };
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      t.accepted = [false, false];
      t.version++;
      t.changedAt = this.clock();
      for (const h of t.heroes) this.host.send(h, { t: 'trade_error', error: e.code, message: e.message });
      this.push(t, true);
      return { done: false, error: e.code };
    }
  }

  /** Untick without changing the offer. */
  unaccept(heroId: number): void {
    const [t, i] = this.find(heroId);
    if (!t.accepted[i]) return;
    t.accepted[i] = false;
    this.push(t);
  }

  cancel(heroId: number, reason: TradeCloseReason = 'cancelled'): void {
    const id = this.byHero.get(heroId);
    const t = id && this.trades.get(id);
    if (t) this.close(t, reason, heroId);
    const inv = this.invites.get(heroId);
    if (inv) {
      this.invites.delete(heroId);
      this.host.send(inv.to, { t: 'trade_invite_gone', from: heroId });
    }
  }

  /** The hero left the world: close their window and forget their invites. */
  left(heroId: number): void {
    this.cancel(heroId, 'left');
    for (const inv of [...this.invites.values()]) if (inv.to === heroId) this.invites.delete(inv.from);
  }

  // ---------------------------------------------------------------- upkeep

  /** Called every server frame: about once a second, close windows that no longer hold. */
  tick(dtMs: number): void {
    this.checkIn -= dtMs;
    if (this.checkIn > 0) return;
    this.checkIn = CHECK_EVERY_MS;
    this.sweep();
  }

  sweep(now = this.clock()): void {
    for (const [from, inv] of [...this.invites]) if (inv.expiresAt < now) this.invites.delete(from);
    for (const t of [...this.trades.values()]) {
      if (t.expiresAt < now) {
        this.close(t, 'expired');
        continue;
      }
      try {
        this.checkPair(t.heroes[0], t.heroes[1], t.id);
      } catch (e) {
        if (!(e instanceof GameError)) throw e;
        this.close(t, e.code === 'too_far' ? 'too_far' : e.code === 'offline' ? 'left' : 'blocked');
      }
    }
  }

  // ---------------------------------------------------------------- internals

  /** Both heroes online, side by side, allowed to trade, and free (or in this trade). */
  private checkPair(a: number, b: number, tradeId?: string): void {
    const wa = this.host.where(a);
    const wb = this.host.where(b);
    if (!wa || !wb) throw new GameError('offline', 'That hero is not here');
    if (wa.channel !== wb.channel || Math.hypot(wa.x - wb.x, wa.y - wb.y, wa.z - wb.z) > TRADE_REACH)
      throw new GameError('too_far', 'Stand closer to trade');
    for (const h of [a, b]) {
      const busy = this.byHero.get(h);
      if (busy && busy !== tradeId) throw new GameError('busy', `${this.host.name(h)} is already trading`);
      const blocked = this.economy.tradeBlock(h);
      if (blocked) throw blocked;
    }
  }

  private validate(heroId: number, offer: Offer): void {
    if (offer.gold > this.economy.balance(heroId)) throw new GameError('insufficient_gold', 'Not enough gold');
    // A whole stack is frozen at the size the other side sees: if it shrinks before the swap
    // (a potion drunk), executeTrade refuses the trade instead of handing over less.
    for (const x of offer.items) {
      const it = this.economy.tradeableItem(heroId, x.id, x.qty);
      x.qty ??= it.quantity;
    }
  }

  private side(t: Trade, i: 0 | 1): TradeSide {
    const o = t.offers[i];
    const qty: Record<string, number> = {};
    for (const x of o.items) if (x.qty !== undefined) qty[x.id] = x.qty;
    return { heroId: t.heroes[i], gold: o.gold, itemIds: o.items.map((x) => x.id), qty };
  }

  private describe(heroId: number, o: Offer): { gold: number; items: TradeItem[] } {
    const items: TradeItem[] = [];
    for (const x of o.items) {
      const it = this.economy.item(x.id);
      if (it && it.owner_hero === heroId) items.push({ item: it.id, def: it.def_id, qty: x.qty ?? it.quantity });
    }
    return { gold: o.gold, items };
  }

  /** What this hero could put in the window: carried items that may be traded. */
  stock(heroId: number): TradeItem[] {
    const out: TradeItem[] = [];
    for (const it of this.economy.inventory(heroId)) {
      if (it.location !== 'inventory') continue;
      try {
        this.economy.tradeableItem(heroId, it.id);
      } catch {
        continue;
      }
      out.push({ item: it.id, def: it.def_id, qty: it.quantity });
    }
    return out;
  }

  /** Send each side the window as they see it. `withStock` adds their tradeable inventory. */
  private push(t: Trade, withStock = false): void {
    const settle = Math.max(0, SETTLE_MS - (this.clock() - t.changedAt));
    for (const i of [0, 1] as const) {
      const me = t.heroes[i];
      const them = t.heroes[1 - i]!;
      this.host.send(me, {
        t: 'trade_state',
        trade: t.id,
        with: them,
        name: this.host.name(them),
        version: t.version,
        settle_ms: settle,
        mine: this.describe(me, t.offers[i]),
        theirs: this.describe(them, t.offers[1 - i]!),
        accepted: { me: t.accepted[i], them: t.accepted[1 - i] },
        gold: this.economy.balance(me),
        ...(withStock ? { stock: this.stock(me) } : {}),
      });
    }
  }

  private find(heroId: number): [Trade, 0 | 1] {
    const id = this.byHero.get(heroId);
    const t = id ? this.trades.get(id) : undefined;
    if (!t || t.expiresAt < this.clock()) throw new GameError('no_trade', 'No open trade');
    return [t, t.heroes[0] === heroId ? 0 : 1];
  }

  private drop(t: Trade): void {
    this.trades.delete(t.id);
    for (const h of t.heroes) if (this.byHero.get(h) === t.id) this.byHero.delete(h);
  }

  private close(t: Trade, reason: TradeCloseReason, by?: number): void {
    this.drop(t);
    for (const h of t.heroes) this.host.send(h, { t: 'trade_closed', trade: t.id, reason, ...(by !== undefined ? { by } : {}) });
  }
}
