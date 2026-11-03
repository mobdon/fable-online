// The one object the game server talks to. Each call updates the Prometheus
// metrics, writes an event to the log, and (for the economy) runs the watchdog.

import { ANOMALY_RULES, EconomyWatchdog, type Anomaly, type WatchdogConfig } from "./economy.js";
import { MemorySink, type EventSink } from "./events.js";
import {
  ABILITIES, COUNTERS, GOLD_SINKS, GOLD_SOURCES, PVP_VENUES, SCHOOLS,
  bounded, createMetrics, type Metrics, type PvpVenue, type School,
} from "./metrics.js";

export interface AnalyticsOptions {
  sink?: EventSink;
  watchdog?: Partial<WatchdogConfig>;
  /** Called for every anomaly, e.g. to alert GMs or freeze a hero. */
  onAnomaly?: (a: Anomaly) => void;
  now?: () => number;
  defaultMetrics?: boolean;
}

export class Analytics {
  readonly metrics: Metrics;
  readonly watchdog: EconomyWatchdog;
  readonly sink: EventSink;
  private readonly now: () => number;
  private readonly onAnomaly?: (a: Anomaly) => void;
  private ledgerSupply = 0;
  private uniqueDay = "";
  private uniqueAccounts = new Set<string>();
  private online = new Map<string, { region: string; since: number }>();

  constructor(opts: AnalyticsOptions = {}) {
    this.now = opts.now ?? Date.now;
    this.metrics = createMetrics({ defaultMetrics: opts.defaultMetrics });
    this.watchdog = new EconomyWatchdog(opts.watchdog, this.now);
    this.sink = opts.sink ?? new MemorySink();
    this.onAnomaly = opts.onAnomaly;
    // Start every rule at 0 so Prometheus' increase() sees the very first anomaly.
    for (const rule of ANOMALY_RULES) this.metrics.economyAnomalies.inc({ rule }, 0);
  }

  private emit(type: string, fields: Record<string, unknown>) {
    this.sink.write({ ts: new Date(this.now()).toISOString(), type, ...fields });
  }

  private report(anomalies: Anomaly[]) {
    for (const a of anomalies) {
      this.metrics.economyAnomalies.inc({ rule: a.rule });
      this.emit("economy.anomaly", { rule: a.rule, playerId: a.playerId, ...a.detail });
      this.onAnomaly?.(a);
    }
    this.metrics.economyFlaggedPlayers.set(this.watchdog.flaggedCount());
    return anomalies;
  }

  // ---- players ---------------------------------------------------------------

  login(accountId: string, result: "ok" | "bad_password" | "banned" | "version_mismatch" | "server_full") {
    this.metrics.logins.inc({ result });
    this.emit("session.login", { accountId, result });
    if (result !== "ok") return;
    const day = new Date(this.now()).toISOString().slice(0, 10);
    if (day !== this.uniqueDay) { this.uniqueDay = day; this.uniqueAccounts.clear(); }
    this.uniqueAccounts.add(accountId);
    this.metrics.playersUniqueToday.set(this.uniqueAccounts.size);
  }

  /** Hero enters a shared-world region (first spawn, or after a transfer). */
  enterRegion(playerId: string, region: string, via: "login" | "walk" | "cullis_gate" | "respawn" = "login") {
    const prev = this.online.get(playerId);
    if (prev) {
      if (prev.region === region) return;
      this.metrics.playersOnline.dec({ region: prev.region });
      this.metrics.regionTransfers.inc({ from: prev.region, to: region, via });
      if (via === "cullis_gate") this.metrics.cullisTeleports.inc({ from: prev.region, to: region });
    }
    this.online.set(playerId, { region, since: prev?.since ?? this.now() });
    this.metrics.playersOnline.inc({ region });
    this.emit("player.enter_region", { playerId, region, from: prev?.region, via });
  }

  logout(playerId: string) {
    const s = this.online.get(playerId);
    if (!s) return;
    this.online.delete(playerId);
    this.metrics.playersOnline.dec({ region: s.region });
    const seconds = (this.now() - s.since) / 1000;
    this.metrics.sessionSeconds.observe(seconds);
    this.emit("session.logout", { playerId, region: s.region, seconds });
  }

  setTutorialCount(n: number) { this.metrics.playersInTutorial.set(n); }

  graduate(playerId: string) {
    this.metrics.tutorialGraduations.inc();
    this.emit("player.graduated", { playerId });
  }

  // ---- economy ---------------------------------------------------------------

  /** Gold created out of nothing (quest, loot, shop sale...). */
  goldMinted(c: { playerId: string; amount: number; source: string; region: string; txnId?: string; balanceBefore?: number; balanceAfter?: number }) {
    const amount = Math.abs(c.amount);
    this.metrics.goldMinted.inc({ source: bounded(c.source, GOLD_SOURCES), region: c.region }, amount);
    this.metrics.goldTxSize.observe({ kind: "mint" }, amount);
    this.ledgerSupply += amount;
    this.metrics.goldSupplyLedger.set(this.ledgerSupply);
    this.emit("economy.gold", { kind: "mint", ...c, amount });
    return this.report(this.watchdog.checkGold({ ...c, delta: amount, kind: "mint", reason: c.source }));
  }

  /** Gold destroyed (shop purchase, training, death...). */
  goldBurned(c: { playerId: string; amount: number; sink: string; region: string; txnId?: string; balanceBefore?: number; balanceAfter?: number }) {
    const amount = Math.abs(c.amount);
    this.metrics.goldBurned.inc({ sink: bounded(c.sink, GOLD_SINKS), region: c.region }, amount);
    this.metrics.goldTxSize.observe({ kind: "burn" }, amount);
    this.ledgerSupply -= amount;
    this.metrics.goldSupplyLedger.set(this.ledgerSupply);
    this.emit("economy.gold", { kind: "burn", ...c, amount });
    return this.report(this.watchdog.checkGold({ ...c, delta: amount, kind: "burn", reason: c.sink }));
  }

  /** Gold moving between two heroes. Doesn't change supply. */
  goldTransferred(c: { fromPlayerId: string; toPlayerId: string; amount: number; kind: "trade" | "mail" | "guild"; txnId?: string }) {
    const amount = Math.abs(c.amount);
    this.metrics.goldTransferred.inc({ kind: c.kind }, amount);
    this.metrics.goldTxSize.observe({ kind: "transfer" }, amount);
    this.emit("economy.transfer", { ...c, amount });
    return this.report(this.watchdog.checkTransfer({ ...c, amount }));
  }

  /** Gold in the world according to the ledger. */
  get goldSupply() { return this.ledgerSupply; }

  /** Call on startup (with the DB total) so the ledger gauge starts from the truth. */
  setLedgerSupply(total: number) {
    this.ledgerSupply = total;
    this.metrics.goldSupplyLedger.set(total);
  }

  /** Call periodically with SUM(gold) from the database. A gap means gold moved off-ledger. */
  reportDbSupply(total: number) {
    this.metrics.goldSupplyDb.set(total);
    return this.report(this.watchdog.checkSupply(this.ledgerSupply, total));
  }

  itemCreated(c: { playerId: string; itemDef: string; category: string; source: string; count?: number }) {
    this.metrics.itemsCreated.inc({ source: c.source, category: c.category }, c.count ?? 1);
    this.emit("economy.item", { kind: "create", ...c });
  }

  itemDestroyed(c: { playerId: string; itemDef: string; category: string; sink: string; count?: number }) {
    this.metrics.itemsDestroyed.inc({ sink: c.sink, category: c.category }, c.count ?? 1);
    this.emit("economy.item", { kind: "destroy", ...c });
  }

  phialCreated(playerId: string, source: string) {
    this.metrics.phialsCreated.inc({ source });
    this.emit("economy.phial", { kind: "create", playerId, source });
    return this.report(this.watchdog.checkPhial(playerId));
  }

  phialUsed(playerId: string, venue: PvpVenue | "pve") {
    this.metrics.phialsUsed.inc({ venue });
    this.emit("economy.phial", { kind: "use", playerId, venue });
  }

  // ---- PvP & Arena -------------------------------------------------------------

  pvpHit(c: { venue: PvpVenue; attackerId: string; victimId: string; ability: string; damage: number; countered?: string }) {
    const venue = bounded(c.venue, PVP_VENUES);
    const ability = bounded(c.ability, ABILITIES);
    this.metrics.pvpAbilityUses.inc({ venue, ability });
    if (c.countered) this.metrics.pvpCounters.inc({ ability, counter: bounded(c.countered, COUNTERS) });
    else if (c.damage > 0) this.metrics.pvpDamage.inc({ venue, ability }, c.damage);
  }

  pvpKill(c: { venue: PvpVenue; killerId: string; victimId: string; killerSchool: School; victimSchool: School; secondsToKill: number; region: string }) {
    const venue = bounded(c.venue, PVP_VENUES);
    const killer_school = bounded(c.killerSchool, SCHOOLS);
    this.metrics.pvpKills.inc({ venue, killer_school, victim_school: bounded(c.victimSchool, SCHOOLS) });
    this.metrics.pvpTimeToKill.observe({ venue, killer_school }, c.secondsToKill);
    this.emit("pvp.kill", c);
  }

  arenaMatch(c: { matchId: string; mode: "1v1" | "2v2" | "ffa" | "wave"; seconds: number; players: Array<{ playerId: string; school: School; result: "win" | "loss" | "draw"; queueSeconds?: number }> }) {
    this.metrics.arenaMatches.inc({ mode: c.mode });
    this.metrics.arenaMatchSeconds.observe({ mode: c.mode }, c.seconds);
    for (const p of c.players) {
      this.metrics.arenaResults.inc({ mode: c.mode, school: bounded(p.school, SCHOOLS), result: p.result });
      if (p.queueSeconds !== undefined) this.metrics.arenaQueueSeconds.observe({ mode: c.mode }, p.queueSeconds);
    }
    this.emit("arena.match", c);
  }

  setPvpFlaggedCount(n: number) { this.metrics.pvpFlagged.set(n); }

  // ---- social ----------------------------------------------------------------

  chat(channel: "say" | "region" | "whisper" | "guild" | "trade" | "arena") { this.metrics.chatMessages.inc({ channel }); }

  playerReport(reporterId: string, targetId: string, reason: "spam" | "abuse" | "cheating" | "other") {
    this.metrics.chatReports.inc({ reason });
    this.emit("social.report", { reporterId, targetId, reason });
  }

  // ---- server health ---------------------------------------------------------

  regionProcess(region: string, up: boolean, restartReason?: string) {
    this.metrics.regionUp.set({ region }, up ? 1 : 0);
    if (restartReason) {
      this.metrics.regionRestarts.inc({ region, reason: restartReason });
      this.emit("server.region_restart", { region, reason: restartReason });
    }
  }

  tick(region: string, seconds: number) { this.metrics.tickSeconds.observe({ region }, seconds); }

  net(direction: "in" | "out", type: string, bytes: number) {
    this.metrics.netMessages.inc({ direction, type });
    this.metrics.netBytes.inc({ direction }, bytes);
  }

  /** Wrap a DB call to time it: `await analytics.timeDb("load_hero", () => db.query(...))` */
  async timeDb<T>(op: string, fn: () => Promise<T>): Promise<T> {
    const end = this.metrics.dbQuerySeconds.startTimer({ op });
    try { return await fn(); } finally { end(); }
  }

  error(service: string, kind: string, detail?: unknown) {
    this.metrics.errors.inc({ service, kind });
    this.emit("server.error", { service, kind, detail: detail instanceof Error ? detail.message : detail });
  }
}

export type { Anomaly };
