// Economy watchdog: checks every gold / item change against simple rules and
// reports anything that looks like a money loophole or a dupe.
//
// It never blocks a transaction by itself. The game server decides what to do with
// a flag (log it, freeze the hero, page a GM). Keeping it advisory means a bad
// threshold can't break the game.

import { GOLD_SINKS, GOLD_SOURCES } from "./metrics.js";

export const ANOMALY_RULES = [
  "unknown_source", "unknown_sink", "duplicate_txn", "large_delta", "earn_rate",
  "negative_balance", "balance_mismatch", "supply_drift", "phial_rate", "self_trade",
] as const;

export type AnomalyRule =
  | "unknown_source"     // gold minted from a source that isn't on the allowed list
  | "unknown_sink"       // gold burned through a sink that isn't on the allowed list
  | "duplicate_txn"      // the same transaction id applied twice (replayed packet, dupe bug)
  | "large_delta"        // one transaction far bigger than that source should ever pay
  | "earn_rate"          // one hero minting too much gold in a short window
  | "negative_balance"   // a balance went below zero
  | "balance_mismatch"   // before + delta != after: something changed the balance off-ledger
  | "supply_drift"       // sum of all balances in the DB != what the ledger says exists
  | "phial_rate"         // one hero getting too many Resurrection Phials
  | "self_trade";        // a hero trading with themselves (alt-account laundering shows up as rate)

export interface Anomaly {
  rule: AnomalyRule;
  playerId?: string;
  detail: Record<string, unknown>;
}

export interface WatchdogConfig {
  /** Biggest single mint allowed per source before it is flagged. */
  maxSingleMint: Partial<Record<string, number>> & { default: number };
  /** Window for earn_rate, in ms. */
  earnWindowMs: number;
  /** Most gold one hero may mint inside earnWindowMs. */
  maxEarnPerWindow: number;
  /** Sources that don't count toward earn_rate (e.g. GM grants are audited elsewhere). */
  earnRateExempt: string[];
  /** Window and limit for phial_rate. */
  phialWindowMs: number;
  maxPhialsPerWindow: number;
  /** How many recent transaction ids to remember for duplicate_txn. */
  txnMemory: number;
  /** Allowed gap between ledger and DB supply before supply_drift fires. */
  supplyTolerance: number;
}

export const DEFAULT_WATCHDOG_CONFIG: WatchdogConfig = {
  // Draft numbers. Fable quest rewards top out in the low thousands; the Arena pays more.
  maxSingleMint: { default: 20_000, arena_prize: 50_000, quest_reward: 30_000, gm_grant: Infinity },
  earnWindowMs: 60 * 60 * 1000,
  maxEarnPerWindow: 100_000,
  earnRateExempt: ["gm_grant"],
  phialWindowMs: 24 * 60 * 60 * 1000,
  maxPhialsPerWindow: 2,
  txnMemory: 200_000,
  supplyTolerance: 0,
};

const FLAG_TTL_MS = 24 * 60 * 60 * 1000;

export class EconomyWatchdog {
  readonly config: WatchdogConfig;
  private earned = new Map<string, Array<[number, number]>>();
  private phials = new Map<string, number[]>();
  private seenTxns = new Set<string>();
  private txnOrder: string[] = [];
  private flagged = new Map<string, number>();

  constructor(config: Partial<WatchdogConfig> = {}, private readonly now: () => number = Date.now) {
    this.config = { ...DEFAULT_WATCHDOG_CONFIG, ...config };
  }

  /** Returns true the first time a txn id is seen. Ids are remembered FIFO up to txnMemory. */
  private firstSeen(txnId: string): boolean {
    if (this.seenTxns.has(txnId)) return false;
    this.seenTxns.add(txnId);
    this.txnOrder.push(txnId);
    if (this.txnOrder.length > this.config.txnMemory) {
      this.seenTxns.delete(this.txnOrder.shift()!);
    }
    return true;
  }

  checkGold(c: {
    playerId: string; delta: number; kind: "mint" | "burn"; reason: string;
    txnId?: string; balanceBefore?: number; balanceAfter?: number;
  }): Anomaly[] {
    const out: Anomaly[] = [];
    const p = c.playerId;
    if (c.txnId && !this.firstSeen(c.txnId)) out.push({ rule: "duplicate_txn", playerId: p, detail: { txnId: c.txnId, delta: c.delta } });

    if (c.kind === "mint") {
      if (!(GOLD_SOURCES as readonly string[]).includes(c.reason)) out.push({ rule: "unknown_source", playerId: p, detail: { source: c.reason, delta: c.delta } });
      const limit = this.config.maxSingleMint[c.reason] ?? this.config.maxSingleMint.default;
      if (Math.abs(c.delta) > limit) out.push({ rule: "large_delta", playerId: p, detail: { source: c.reason, delta: c.delta, limit } });
      if (!this.config.earnRateExempt.includes(c.reason)) {
        const total = this.addToWindow(p, Math.abs(c.delta));
        if (total > this.config.maxEarnPerWindow && total - Math.abs(c.delta) <= this.config.maxEarnPerWindow) {
          // Fire once when the hero crosses the line, not on every later transaction.
          out.push({ rule: "earn_rate", playerId: p, detail: { earnedInWindow: total, windowMs: this.config.earnWindowMs, limit: this.config.maxEarnPerWindow } });
        }
      }
    } else if (!(GOLD_SINKS as readonly string[]).includes(c.reason)) {
      out.push({ rule: "unknown_sink", playerId: p, detail: { sink: c.reason, delta: c.delta } });
    }

    if (c.balanceAfter !== undefined && c.balanceAfter < 0) out.push({ rule: "negative_balance", playerId: p, detail: { balanceAfter: c.balanceAfter } });
    if (c.balanceBefore !== undefined && c.balanceAfter !== undefined) {
      const signed = c.kind === "mint" ? Math.abs(c.delta) : -Math.abs(c.delta);
      if (c.balanceBefore + signed !== c.balanceAfter) {
        out.push({ rule: "balance_mismatch", playerId: p, detail: { before: c.balanceBefore, delta: signed, after: c.balanceAfter } });
      }
    }
    this.remember(out);
    return out;
  }

  checkTransfer(c: { fromPlayerId: string; toPlayerId: string; amount: number; txnId?: string }): Anomaly[] {
    const out: Anomaly[] = [];
    if (c.txnId && !this.firstSeen(c.txnId)) out.push({ rule: "duplicate_txn", playerId: c.fromPlayerId, detail: { txnId: c.txnId, amount: c.amount, to: c.toPlayerId } });
    if (c.fromPlayerId === c.toPlayerId) out.push({ rule: "self_trade", playerId: c.fromPlayerId, detail: { amount: c.amount } });
    this.remember(out);
    return out;
  }

  checkPhial(playerId: string): Anomaly[] {
    const now = this.now();
    const list = (this.phials.get(playerId) ?? []).filter((t) => now - t < this.config.phialWindowMs);
    list.push(now);
    this.phials.set(playerId, list);
    const out: Anomaly[] = list.length > this.config.maxPhialsPerWindow
      ? [{ rule: "phial_rate", playerId, detail: { phialsInWindow: list.length, limit: this.config.maxPhialsPerWindow } }]
      : [];
    this.remember(out);
    return out;
  }

  checkSupply(ledgerSupply: number, dbSupply: number): Anomaly[] {
    const drift = dbSupply - ledgerSupply;
    return Math.abs(drift) > this.config.supplyTolerance
      ? [{ rule: "supply_drift", detail: { ledgerSupply, dbSupply, drift } }]
      : [];
  }

  /** Distinct heroes flagged in the last 24h. */
  flaggedCount(): number {
    const now = this.now();
    for (const [p, t] of this.flagged) if (now - t > FLAG_TTL_MS) this.flagged.delete(p);
    return this.flagged.size;
  }

  private addToWindow(playerId: string, amount: number): number {
    const now = this.now();
    const list = (this.earned.get(playerId) ?? []).filter(([t]) => now - t < this.config.earnWindowMs);
    list.push([now, amount]);
    this.earned.set(playerId, list);
    return list.reduce((s, [, a]) => s + a, 0);
  }

  private remember(anomalies: Anomaly[]) {
    const now = this.now();
    for (const a of anomalies) if (a.playerId) this.flagged.set(a.playerId, now);
  }
}
