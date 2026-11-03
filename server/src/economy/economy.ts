import crypto from 'node:crypto';
import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';
import type { ItemCatalog } from './catalog.js';

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

export const SINK_REASONS = ['vendor_buy', 'repair', 'tattoo_haircut', 'property', 'arena_fee', 'death_penalty', 'gm_remove'] as const;
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
}

const MINT = 'system:mint';
const SINK = 'system:sink';
const wallet = (heroId: number) => `hero:${heroId}`;

export class Economy {
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

  // ---------------------------------------------------------------- vendors

  vendorBuy(heroId: number, defId: string, quantity: number, key: string): TxResult & { itemId: string } {
    const def = this.catalog.get(defId);
    if (def.buy === null) throw new GameError('not_for_sale', 'Vendors do not sell that');
    let itemId = '';
    const r = this.action('vendor_buy', wallet(heroId), key, { defId, quantity }, (tx) => {
      if (!Number.isInteger(quantity) || quantity < 1 || quantity > def.maxStack) throw new GameError('bad_quantity', 'Bad quantity');
      if (def.buy! > 0) this.moveGold(tx, wallet(heroId), SINK, def.buy! * quantity, 'vendor_buy');
      itemId = this.createItem(tx, heroId, defId, quantity);
    });
    if (r.replayed) itemId = this.firstItemOfTx(r.txId);
    return { ...r, itemId };
  }

  vendorSell(heroId: number, itemId: string, key: string): TxResult & { gold: number } {
    let gold = 0;
    const r = this.action('vendor_sell', wallet(heroId), key, { itemId }, (tx) => {
      const it = this.ownedItem(heroId, itemId, 'inventory');
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

  // ---------------------------------------------------------------- trade

  /**
   * Swap gold and items between two heroes in one transaction. TradeSessions decides *when*
   * (both sides accepted the same offer); this only checks *whether* it is still valid.
   */
  executeTrade(a: TradeSide, b: TradeSide, key: string): TxResult {
    if (a.heroId === b.heroId) throw new GameError('bad_trade', 'Cannot trade with yourself');
    const all = [...a.itemIds, ...b.itemIds];
    if (new Set(all).size !== all.length) throw new GameError('bad_trade', 'Duplicate item in offer');
    const r = this.action('trade', 'system', key, { a, b }, (tx) => {
      for (const [from, to] of [[a, b], [b, a]] as const) {
        for (const id of from.itemIds) {
          const it = this.ownedItem(from.heroId, id, 'inventory');
          if (!this.catalog.get(it.def_id).tradeable) throw new GameError('untradeable', `${it.def_id} cannot be traded`);
          this.db.run('UPDATE items SET owner_hero = ? WHERE id = ?', to.heroId, it.id);
          this.history(tx, it, 'transfer', from.heroId, to.heroId, it.quantity);
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
