import crypto from 'node:crypto';
import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';
import { MAX_SELL_RATIO, type ItemCatalog } from './catalog.js';

/**
 * Gold and items, server-authoritative, double-entry, idempotent.
 *
 * Rules that close the usual MMO money loopholes:
 *  - Gold only enters through `system:mint` with a whitelisted reason and a per-action cap,
 *    and only leaves through `system:sink`. SUM(all wallet balances) is always 0, so the
 *    total gold in the world is exactly -balance(system:mint) - balance(system:sink).
 *  - Hero wallets cannot go negative (DB CHECK constraint, plus a guarded UPDATE).
 *  - Amounts are positive integers below MAX_AMOUNT. No floats, no overflow.
 *  - Every action carries an idempotency key. A retried or doubled request is a no-op that
 *    returns the first result, so lag or a double-click can never pay twice.
 *  - Every item has a random 128-bit ID that is never reused. Items are never copied, only
 *    moved; split and merge conserve quantity and are recorded in item_history.
 *  - Prices come from the server's ItemCatalog, never from a client.
 *  - Everything for one action runs in one transaction: it all happens or none of it does.
 */

export const MAX_AMOUNT = 1_000_000_000;

/** Where gold may come from, and the most one action may create. */
export const MINT_REASONS = {
  quest_reward: 50_000,
  loot: 5_000,
  vendor_sell: 1_000_000,
  job_wage: 2_000,
  gm_grant: 10_000_000,
} as const;
export type MintReason = keyof typeof MINT_REASONS;

export const SINK_REASONS = ['vendor_buy', 'repair', 'tattoo_haircut', 'property', 'arena_fee', 'death_penalty', 'crime', 'gm_remove'] as const;
export type SinkReason = (typeof SINK_REASONS)[number];

export type ItemLocation = 'inventory' | 'equipped' | 'bank';

export interface ItemRow {
  id: string;
  def_id: string;
  quantity: number;
  owner_hero: number | null;
  location: string;
  created_tx: number;
  destroyed_tx: number | null;
}

export interface TxResult {
  txId: number;
  /** true when this idempotency key was already used: nothing was done again. */
  replayed: boolean;
}

export interface TradeSide {
  heroId: number;
  gold: number;
  itemIds: string[];
  /** Part of a stack: item ID -> how many to hand over. Left out, the whole stack goes. */
  qty?: Record<string, number>;
}

/**
 * Extra checks other systems hang on the economy without it depending on them. The law
 * module uses them to keep stolen goods away from normal traders and other players, and
 * to refuse shop trades while a hero is wanted. Each returns an error to refuse, or null.
 */
export interface EconomyPolicy {
  /** May this item be sold to a normal vendor ('sell') or handed to another hero ('trade')? */
  item?(itemId: string, use: 'sell' | 'trade'): GameError | null;
  /** May this hero use a shop, or trade with another hero, right now? */
  service?(heroId: number, service: 'shop' | 'trade'): GameError | null;
}

const MINT = 'system:mint';
const SINK = 'system:sink';
const wallet = (heroId: number) => `hero:${heroId}`;

export class Economy {
  readonly policies: EconomyPolicy[] = [];

  constructor(
    private db: Database,
    private events: EventSink,
    readonly catalog: ItemCatalog,
  ) {
    db.run("INSERT OR IGNORE INTO wallets (id, kind) VALUES (?, 'system'), (?, 'system')", MINT, SINK);
  }

  // ---------------------------------------------------------------- gold

  balance(heroId: number): number {
    const w = this.db.get<{ balance: number }>('SELECT balance FROM wallets WHERE id = ?', wallet(heroId));
    if (!w) throw new GameError('no_wallet', 'Hero has no wallet', 404);
    return w.balance;
  }

  /** Create gold for a hero (quest reward, loot, etc.). */
  grant(heroId: number, amount: number, reason: MintReason, key: string, actor = 'system'): TxResult {
    const cap = MINT_REASONS[reason];
    if (cap === undefined) throw new GameError('bad_reason', `Gold cannot be created for "${reason}"`);
    if (amount > cap) throw new GameError('over_cap', `${reason} is capped at ${cap} gold per action`);
    if (reason === 'gm_grant' && !actor.startsWith('gm:')) throw new GameError('forbidden', 'GM only', 403);
    return this.action('grant', actor, key, { heroId, amount, reason }, (tx) => {
      this.moveGold(tx, MINT, wallet(heroId), amount, reason);
    });
  }

  /** Destroy gold a hero spends on a service. */
  spend(heroId: number, amount: number, reason: SinkReason, key: string, actor = `hero:${heroId}`): TxResult {
    if (!SINK_REASONS.includes(reason)) throw new GameError('bad_reason', `Unknown sink "${reason}"`);
    return this.action('spend', actor, key, { heroId, amount, reason }, (tx) => {
      this.moveGold(tx, wallet(heroId), SINK, amount, reason);
    });
  }

  // ---------------------------------------------------------------- items

  item(itemId: string): ItemRow | undefined {
    return this.db.get<ItemRow>('SELECT * FROM items WHERE id = ?', itemId);
  }

  inventory(heroId: number): ItemRow[] {
    return this.db.all<ItemRow>("SELECT * FROM items WHERE owner_hero = ? AND location != 'destroyed' ORDER BY id", heroId);
  }

  /** Create an item from nothing (loot, quest reward). Returns the new item's ID. */
  mintItem(heroId: number, defId: string, quantity: number, reason: string, key: string, actor = 'system'): TxResult & { itemId: string } {
    if (this.catalog.get(defId).staffOnly && !actor.startsWith('gm:')) {
      this.events.emit({ type: 'staff_item_blocked', heroId, defId, action: `mint:${reason}`, actor });
      throw new GameError('staff_only', `${defId} is staff-only gear`, 403);
    }
    let itemId = '';
    const r = this.action('mint_item', actor, key, { heroId, defId, quantity, reason }, (tx) => {
      itemId = this.createItem(tx, heroId, defId, quantity);
    });
    if (r.replayed) itemId = this.firstItemOfTx(r.txId);
    return { ...r, itemId };
  }

  /** Move an item between the owner's own inventory, equipment and bank. */
  moveItem(heroId: number, itemId: string, to: ItemLocation, key: string): TxResult {
    if (!['inventory', 'equipped', 'bank'].includes(to)) throw new GameError('bad_location', 'Bad location');
    return this.action('move_item', wallet(heroId), key, { itemId, to }, (tx) => {
      const it = this.ownedItem(heroId, itemId);
      this.db.run('UPDATE items SET location = ? WHERE id = ?', to, it.id);
      this.history(tx, it, 'move', heroId, heroId, it.quantity);
    });
  }

  /**
   * Use up one of an item from the hero's carried inventory (a potion drunk, food eaten).
   * Takes from the smallest stack. Throws `not_owner` when the hero carries none.
   */
  consumeOne(heroId: number, defId: string, key: string): TxResult & { itemId: string } {
    let used = '';
    const r = this.action('consume', wallet(heroId), key, { defId }, (tx) => {
      const it = this.db.get<ItemRow>(
        "SELECT * FROM items WHERE owner_hero = ? AND def_id = ? AND location = 'inventory' AND quantity > 0 ORDER BY quantity, rowid LIMIT 1",
        heroId,
        defId,
      );
      if (!it) throw new GameError('not_owner', 'You do not have that item');
      used = it.id;
      if (it.quantity > 1) this.db.run('UPDATE items SET quantity = quantity - 1 WHERE id = ?', it.id);
      else this.db.run("UPDATE items SET location = 'destroyed', destroyed_tx = ? WHERE id = ?", tx, it.id);
      // Logged as a destroy of one: item_history's actions are fixed by a CHECK in the schema.
      this.history(tx, it, 'destroy', heroId, null, -1);
    });
    return { ...r, itemId: used };
  }

  /** Split `quantity` off a stack into a brand-new item ID. */
  splitStack(heroId: number, itemId: string, quantity: number, key: string): TxResult & { itemId: string } {
    let newId = '';
    const r = this.action('split', wallet(heroId), key, { itemId, quantity }, (tx) => {
      const it = this.ownedItem(heroId, itemId);
      if (!Number.isInteger(quantity) || quantity < 1 || quantity >= it.quantity)
        throw new GameError('bad_quantity', 'Split must leave at least one in each stack');
      this.db.run('UPDATE items SET quantity = quantity - ? WHERE id = ?', quantity, it.id);
      this.history(tx, it, 'split', heroId, heroId, -quantity);
      newId = this.insertItem(tx, heroId, it.def_id, quantity, it.location, 'split');
    });
    if (r.replayed) newId = this.firstItemOfTx(r.txId);
    return { ...r, itemId: newId };
  }

  /** Merge stack `fromId` into `intoId`. `fromId` is destroyed; total quantity is unchanged. */
  mergeStacks(heroId: number, fromId: string, intoId: string, key: string): TxResult {
    return this.action('merge', wallet(heroId), key, { fromId, intoId }, (tx) => {
      if (fromId === intoId) throw new GameError('bad_merge', 'Cannot merge a stack into itself');
      const a = this.ownedItem(heroId, fromId);
      const b = this.ownedItem(heroId, intoId);
      if (a.def_id !== b.def_id) throw new GameError('bad_merge', 'Different items');
      if (a.quantity + b.quantity > this.catalog.get(a.def_id).maxStack) throw new GameError('bad_merge', 'Stack would be too big');
      this.db.run("UPDATE items SET location = 'destroyed', destroyed_tx = ? WHERE id = ?", tx, a.id);
      this.history(tx, a, 'merge', heroId, null, -a.quantity);
      this.db.run('UPDATE items SET quantity = quantity + ? WHERE id = ?', a.quantity, b.id);
      this.history(tx, b, 'merge', heroId, heroId, a.quantity);
    });
  }

  destroyItem(heroId: number, itemId: string, key: string): TxResult {
    return this.action('destroy_item', wallet(heroId), key, { itemId }, (tx) => {
      const it = this.ownedItem(heroId, itemId);
      this.db.run("UPDATE items SET location = 'destroyed', destroyed_tx = ? WHERE id = ?", tx, it.id);
      this.history(tx, it, 'destroy', heroId, null, -it.quantity);
    });
  }

  // ---------------------------------------------------------------- world loot

  /**
   * Hand a hero the contents of a chest, container, digging spot or pickup, all in one
   * transaction. `key` is the claim: it encodes who may take what and how often (for
   * example once per hero per chest), so a second claim with the same key is a replay
   * and creates nothing. The ledger row is the record that the hero took it.
   */
  claimLoot(heroId: number, key: string, gold: number, items: { defId: string; quantity: number }[], source: string): TxResult & { itemIds: string[] } {
    const itemIds: string[] = [];
    if (gold > MINT_REASONS.loot) throw new GameError('over_cap', `loot is capped at ${MINT_REASONS.loot} gold per action`);
    const r = this.action('loot', 'system', key, { heroId, gold, items, source }, (tx) => {
      if (!this.db.get('SELECT 1 FROM heroes WHERE id = ?', heroId)) throw new GameError('no_hero', 'Hero not found', 404);
      if (gold > 0) this.moveGold(tx, MINT, wallet(heroId), gold, 'loot');
      for (const it of items) itemIds.push(this.createItem(tx, heroId, it.defId, it.quantity));
    });
    if (r.replayed) for (const row of this.db.all<{ id: string }>('SELECT id FROM items WHERE created_tx = ? ORDER BY rowid', r.txId)) itemIds.push(row.id);
    return { ...r, itemIds };
  }

  /** True if this claim key has been used (the hero already took that loot). */
  claimed(key: string): boolean {
    return this.db.get('SELECT 1 FROM ledger_tx WHERE idempotency_key = ?', key) !== undefined;
  }

  /** How many of an item the hero holds (inventory, equipped and bank). */
  countOf(heroId: number, defId: string): number {
    return this.db.get<{ n: number }>("SELECT COALESCE(SUM(quantity),0) AS n FROM items WHERE owner_hero = ? AND def_id = ? AND location IN ('inventory','equipped','bank')", heroId, defId)!.n;
  }

  // ---------------------------------------------------------------- vendors

  /**
   * Buy from a vendor. `unitPrice` lets a shop charge its markup; it can never be below the
   * catalog price, so no shop can sell under what vendors pay back.
   */
  vendorBuy(heroId: number, defId: string, quantity: number, key: string, unitPrice?: number): TxResult & { itemId: string } {
    this.checkService(heroId, 'shop');
    const def = this.catalog.get(defId);
    if (def.buy === null) throw new GameError('not_for_sale', 'Vendors do not sell that');
    const price = unitPrice ?? def.buy;
    if (!Number.isInteger(price) || price < def.buy) throw new GameError('bad_price', 'Bad price');
    let itemId = '';
    const r = this.action('vendor_buy', wallet(heroId), key, { defId, quantity, price }, (tx) => {
      if (!Number.isInteger(quantity) || quantity < 1 || quantity > def.maxStack) throw new GameError('bad_quantity', 'Bad quantity');
      if (price > 0) this.moveGold(tx, wallet(heroId), SINK, price * quantity, 'vendor_buy');
      itemId = this.createItem(tx, heroId, defId, quantity);
    });
    if (r.replayed) itemId = this.firstItemOfTx(r.txId);
    return { ...r, itemId };
  }

  vendorSell(heroId: number, itemId: string, key: string): TxResult & { gold: number } {
    let gold = 0;
    this.checkService(heroId, 'shop');
    const r = this.action('vendor_sell', wallet(heroId), key, { itemId }, (tx) => {
      const it = this.ownedItem(heroId, itemId, 'inventory');
      this.checkItem(it.id, 'sell');
      const def = this.catalog.get(it.def_id);
      if (def.sell === null) throw new GameError('not_buyable', 'Vendors will not buy that');
      gold = def.sell * it.quantity;
      this.db.run("UPDATE items SET location = 'destroyed', destroyed_tx = ? WHERE id = ?", tx, it.id);
      this.history(tx, it, 'destroy', heroId, null, -it.quantity);
      if (gold > 0) this.moveGold(tx, MINT, wallet(heroId), gold, 'vendor_sell');
    });
    if (r.replayed) gold = this.db.get<{ s: number }>('SELECT COALESCE(SUM(amount),0) AS s FROM ledger_entries WHERE tx_id = ?', r.txId)!.s;
    return { ...r, gold };
  }

  /**
   * Sell an item to a special buyer (the fence) for `ratio` of its vendor price. The ratio
   * can never beat the normal buy/sell rule, so this can't print gold. Item policies are
   * skipped on purpose: the caller is the one buyer allowed to take what they forbid.
   */
  sellAtRatio(heroId: number, itemId: string, ratio: number, key: string, kind = 'fence_sell'): TxResult & { gold: number } {
    if (!(ratio > 0 && ratio <= MAX_SELL_RATIO)) throw new GameError('bad_ratio', `Ratio must be in (0, ${MAX_SELL_RATIO}]`);
    let gold = 0;
    const r = this.action(kind, wallet(heroId), key, { itemId, ratio }, (tx) => {
      const it = this.ownedItem(heroId, itemId, 'inventory');
      const def = this.catalog.get(it.def_id);
      if (def.buy === null) throw new GameError('not_buyable', 'Nobody will buy that');
      gold = Math.floor(def.buy * ratio) * it.quantity;
      this.db.run("UPDATE items SET location = 'destroyed', destroyed_tx = ? WHERE id = ?", tx, it.id);
      this.history(tx, it, 'destroy', heroId, null, -it.quantity);
      if (gold > 0) this.moveGold(tx, MINT, wallet(heroId), gold, 'vendor_sell');
    });
    if (r.replayed) gold = this.db.get<{ s: number }>('SELECT COALESCE(SUM(amount),0) AS s FROM ledger_entries WHERE tx_id = ?', r.txId)!.s;
    return { ...r, gold };
  }

  private checkItem(itemId: string, use: 'sell' | 'trade'): void {
    for (const p of this.policies) {
      const e = p.item?.(itemId, use);
      if (e) throw e;
    }
  }

  private checkService(heroId: number, service: 'shop' | 'trade'): void {
    for (const p of this.policies) {
      const e = p.service?.(heroId, service);
      if (e) throw e;
    }
  }

  // ---------------------------------------------------------------- trade

  /**
   * Why this hero can't trade right now (in prison, wanted), or null. TradeSessions asks
   * before opening a window; executeTrade asks again inside the swap.
   */
  tradeBlock(heroId: number): GameError | null {
    try {
      this.checkService(heroId, 'trade');
      return null;
    } catch (e) {
      if (e instanceof GameError) return e;
      throw e;
    }
  }

  /**
   * Can `heroId` hand over `qty` of this item (whole stack when left out)? Returns the
   * item row, or throws the reason. Same checks executeTrade makes inside the swap.
   */
  tradeableItem(heroId: number, itemId: string, qty?: number): ItemRow {
    const it = this.ownedItem(heroId, itemId, 'inventory');
    const def = this.catalog.get(it.def_id);
    if (def.staffOnly) this.events.emit({ type: 'staff_item_blocked', heroId, defId: it.def_id, action: 'trade', actor: `hero:${heroId}` });
    if (!def.tradeable || def.staffOnly) throw new GameError('untradeable', `${it.def_id} cannot be traded`);
    if (qty !== undefined && (!Number.isInteger(qty) || qty < 1 || qty > it.quantity)) throw new GameError('bad_quantity', 'Bad quantity');
    this.checkItem(it.id, 'trade');
    return it;
  }

  /**
   * Swap gold and items between two heroes in one transaction. TradeSessions decides *when*
   * (both sides accepted the same offer); this checks again *whether* it is still valid:
   * ownership, location, tradeable, stolen marks, prison or wanted, and gold. If any of it
   * fails nothing moves. Part of a stack is split off into a new item ID for the receiver.
   */
  executeTrade(a: TradeSide, b: TradeSide, key: string): TxResult {
    if (a.heroId === b.heroId) throw new GameError('bad_trade', 'Cannot trade with yourself');
    const all = [...a.itemIds, ...b.itemIds];
    if (new Set(all).size !== all.length) throw new GameError('bad_trade', 'Duplicate item in offer');
    for (const s of [a, b]) if (!Number.isSafeInteger(s.gold) || s.gold < 0 || s.gold > MAX_AMOUNT) throw new GameError('bad_amount', 'Bad gold amount');
    const r = this.action('trade', 'system', key, { a, b }, (tx) => {
      for (const s of [a, b]) this.checkService(s.heroId, 'trade');
      for (const [from, to] of [[a, b], [b, a]] as const) {
        for (const id of from.itemIds) {
          const want = from.qty?.[id];
          const it = this.tradeableItem(from.heroId, id, want);
          if (want !== undefined && want < it.quantity) {
            this.db.run('UPDATE items SET quantity = quantity - ? WHERE id = ?', want, it.id);
            this.history(tx, it, 'split', from.heroId, from.heroId, -want);
            const newId = this.insertItem(tx, to.heroId, it.def_id, want, 'inventory', 'split');
            this.history(tx, { ...it, id: newId, quantity: want }, 'transfer', from.heroId, to.heroId, want);
          } else {
            this.db.run('UPDATE items SET owner_hero = ? WHERE id = ?', to.heroId, it.id);
            this.history(tx, it, 'transfer', from.heroId, to.heroId, it.quantity);
          }
        }
        if (from.gold > 0) this.moveGold(tx, wallet(from.heroId), wallet(to.heroId), from.gold, 'trade');
      }
    });
    if (!r.replayed) this.events.emit({ type: 'trade', txId: r.txId, a: a.heroId, b: b.heroId });
    return r;
  }

  // ---------------------------------------------------------------- audit

  /** Whole-economy integrity check. Run on boot and on a schedule; alert on any problem. */
  audit(): { ok: boolean; problems: string[]; supply: { minted: number; sunk: number; circulating: number } } {
    const problems: string[] = [];
    const sum = this.db.get<{ s: number }>('SELECT COALESCE(SUM(balance),0) AS s FROM wallets')!.s;
    if (sum !== 0) problems.push(`wallet balances sum to ${sum}, expected 0`);

    const drift = this.db.all<{ id: string; balance: number; net: number }>(`
      SELECT w.id, w.balance,
        COALESCE((SELECT SUM(amount) FROM ledger_entries WHERE to_wallet = w.id), 0)
      - COALESCE((SELECT SUM(amount) FROM ledger_entries WHERE from_wallet = w.id), 0) AS net
      FROM wallets w`);
    for (const d of drift) if (d.balance !== d.net) problems.push(`${d.id}: balance ${d.balance} but ledger says ${d.net}`);

    const neg = this.db.all<{ id: string }>("SELECT id FROM wallets WHERE kind = 'hero' AND balance < 0");
    for (const n of neg) problems.push(`${n.id} is negative`);

    const qty = this.db.all<{ id: string; quantity: number; hist: number }>(`
      SELECT i.id, i.quantity, COALESCE(SUM(h.quantity),0) AS hist FROM items i
      LEFT JOIN item_history h ON h.item_id = i.id AND h.action IN ('mint','split','merge','destroy')
      WHERE i.location != 'destroyed' GROUP BY i.id`);
    for (const q of qty) if (q.quantity !== q.hist) problems.push(`item ${q.id}: quantity ${q.quantity} but history says ${q.hist}`);

    const orphan = this.db.all<{ id: string }>("SELECT id FROM items WHERE location != 'destroyed' AND owner_hero IS NULL");
    for (const o of orphan) problems.push(`item ${o.id} has no owner`);

    const w = (id: string) => this.db.get<{ balance: number }>('SELECT balance FROM wallets WHERE id = ?', id)!.balance;
    const circulating = this.db.get<{ s: number }>("SELECT COALESCE(SUM(balance),0) AS s FROM wallets WHERE kind = 'hero'")!.s;
    return { ok: problems.length === 0, problems, supply: { minted: -w(MINT), sunk: w(SINK), circulating } };
  }

  // ---------------------------------------------------------------- internals

  /** Open a ledger transaction (or detect a replay) and run body inside one DB transaction. */
  private action(kind: string, actor: string, key: string, meta: unknown, body: (txId: number) => void): TxResult {
    if (typeof key !== 'string' || key.length < 8 || key.length > 128) throw new GameError('bad_key', 'Idempotency key must be 8-128 chars');
    return this.db.tx(() => {
      const prior = this.db.get<{ id: number; kind: string }>('SELECT id, kind FROM ledger_tx WHERE idempotency_key = ?', key);
      if (prior) {
        if (prior.kind !== kind) throw new GameError('key_reused', 'Idempotency key already used for another action', 409);
        return { txId: prior.id, replayed: true };
      }
      const { lastInsertRowid: txId } = this.db.run(
        'INSERT INTO ledger_tx (idempotency_key, kind, actor, meta, created_at) VALUES (?, ?, ?, ?, ?)',
        key,
        kind,
        actor,
        JSON.stringify(meta),
        Date.now(),
      );
      body(txId);
      return { txId, replayed: false };
    });
  }

  private moveGold(tx: number, from: string, to: string, amount: number, reason: string): void {
    if (!Number.isSafeInteger(amount) || amount <= 0 || amount > MAX_AMOUNT) throw new GameError('bad_amount', 'Gold amount must be a positive whole number');
    if (from === to) throw new GameError('bad_amount', 'Same wallet');
    const debit = this.db.run("UPDATE wallets SET balance = balance - ? WHERE id = ? AND (kind = 'system' OR balance >= ?)", amount, from, amount);
    if (debit.changes !== 1) {
      if (!this.db.get('SELECT 1 FROM wallets WHERE id = ?', from)) throw new GameError('no_wallet', `No wallet ${from}`, 404);
      throw new GameError('insufficient_gold', 'Not enough gold');
    }
    const credit = this.db.run('UPDATE wallets SET balance = balance + ? WHERE id = ?', amount, to);
    if (credit.changes !== 1) throw new GameError('no_wallet', `No wallet ${to}`, 404);
    this.db.run('INSERT INTO ledger_entries (tx_id, from_wallet, to_wallet, amount, reason) VALUES (?, ?, ?, ?, ?)', tx, from, to, amount, reason);
    this.events.emit({ type: 'gold', txId: tx, kind: reason, from, to, amount, reason });
  }

  private createItem(tx: number, heroId: number, defId: string, quantity: number): string {
    const def = this.catalog.get(defId);
    if (!Number.isInteger(quantity) || quantity < 1 || quantity > def.maxStack) throw new GameError('bad_quantity', 'Bad quantity');
    if (!this.db.get('SELECT 1 FROM heroes WHERE id = ?', heroId)) throw new GameError('no_hero', 'Hero not found', 404);
    return this.insertItem(tx, heroId, defId, quantity, 'inventory', 'mint');
  }

  private insertItem(tx: number, heroId: number, defId: string, quantity: number, location: string, action: 'mint' | 'split'): string {
    const id = crypto.randomUUID();
    this.db.run('INSERT INTO items (id, def_id, quantity, owner_hero, location, created_tx) VALUES (?, ?, ?, ?, ?, ?)', id, defId, quantity, heroId, location, tx);
    this.db.run('INSERT INTO item_history (item_id, tx_id, action, from_hero, to_hero, quantity) VALUES (?, ?, ?, NULL, ?, ?)', id, tx, action, heroId, quantity);
    this.events.emit({ type: 'item', txId: tx, action, itemId: id, defId, fromHero: null, toHero: heroId, quantity });
    return id;
  }

  private ownedItem(heroId: number, itemId: string, mustBeIn?: ItemLocation): ItemRow {
    const it = this.item(String(itemId));
    if (!it || it.owner_hero !== heroId || it.location === 'destroyed' || it.location === 'escrow')
      throw new GameError('not_owner', 'You do not have that item');
    if (mustBeIn && it.location !== mustBeIn) throw new GameError('wrong_location', `Item must be in your ${mustBeIn}`);
    return it;
  }

  private history(tx: number, it: ItemRow, action: string, from: number | null, to: number | null, quantity: number): void {
    this.db.run('INSERT INTO item_history (item_id, tx_id, action, from_hero, to_hero, quantity) VALUES (?, ?, ?, ?, ?, ?)', it.id, tx, action, from, to, quantity);
    this.events.emit({ type: 'item', txId: tx, action, itemId: it.id, defId: it.def_id, fromHero: from, toHero: to, quantity });
  }

  private firstItemOfTx(txId: number): string {
    return this.db.get<{ id: string }>('SELECT id FROM items WHERE created_tx = ? ORDER BY rowid LIMIT 1', txId)?.id ?? '';
  }
}
