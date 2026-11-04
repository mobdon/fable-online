import crypto from 'node:crypto';
import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import type { Economy } from '../economy/economy.js';
import { GameError } from '../errors.js';
import { DEFAULT_RULES, TEXT, type CrimeKind, type Jurisdiction, type LawRules } from './rules.js';

export type Prison = 'jail' | 'bargate';

export interface CrimeRow {
  id: number;
  hero_id: number;
  kind: CrimeKind;
  town: string | null;
  fine: number;
  level: number;
  victim: string | null;
  online_at: number;
  created_at: number;
  settled: 'paid' | 'bribed' | 'forgiven' | 'served' | null;
}

export interface SentenceRow {
  id: number;
  hero_id: number;
  prison: Prison;
  total_ms: number;
  served_ms: number;
  fines: number;
  reason: string;
  warden_used: number;
  escape_tried: number;
  created_at: number;
  released_at: number | null;
}

/** What the mod needs to draw Fable's fine screen and set its crime memory. */
export interface LawStatus {
  level: number;
  total: number;
  /** One line per crime kind: TEXT_GUI_CRIME_* "%1 x <crime> (%2 gold)". */
  lines: { kind: CrimeKind; textId: string; count: number; gold: number }[];
  /** Minutes of play until the oldest petty crime is forgiven, if any are open. */
  forgivenInMinutes: number | null;
  sentence: { prison: Prison; leftMs: number; region: string } | null;
  kickedFrom: { town: string; leftMs: number }[];
}

/**
 * The world side of the law: where heroes are and how to move them. The app wires this to
 * the Director; tests use a fake.
 */
export interface LawHost {
  /** The region an online hero is in, or undefined when offline. */
  regionOf(heroId: number): string | undefined;
  /** Move an online hero. `private` puts them in a solo copy of the region. */
  relocate(heroId: number, region: string, opts: { private: boolean }): void;
  /** Send a law message to one hero's client. */
  notify(heroId: number, msg: LawMessage): void;
  hasRegion(region: string): boolean;
}

export type LawMessage =
  | ({ t: 'law_status' } & LawStatus)
  | { t: 'law_bark'; guard: number; text: string }
  | { t: 'law_prompt'; kind: 'fine' | 'fine_kick'; guard: number; text: string; fine: number; bribe: number | null; bribeText: string; answerMs: number }
  | { t: 'law_notice'; text: string; gold?: number; minutes?: number }
  | { t: 'law_wanted'; hero: number; level: number };

const DAY_MS = 24 * 3600_000;
const MIN = 60_000;

/**
 * Crime records, fines, bribes, kick-outs and prison sentences. The server is the only
 * authority: clients only report what the player tried to do, guards (GuardDirector) say
 * what they saw, and this decides what it costs. Every gold movement goes through the
 * ledger under the 'crime' sink, so fines and bribes leave the game.
 */
export class Law {
  private online = new Map<number, number>();
  private dirty = new Set<number>();
  /** `${heroId}:${town}` -> real-time ms until that town's guards ignore petty crimes. */
  private blind = new Map<string, number>();
  /** Heroes running from the law: guards everywhere chase them until they are caught or lost. */
  private resisting = new Map<number, number>();
  private flushIn = 10_000;
  private wantedCache = new Map<number, { level: number; at: number }>();

  constructor(
    private db: Database,
    private economy: Economy,
    private events: EventSink,
    readonly rules: LawRules = DEFAULT_RULES,
    private host: LawHost | null = null,
    private clock: () => number = Date.now,
    private rng: () => number = Math.random,
  ) {
    economy.policies.push({
      item: (itemId, use) =>
        this.isMarked(itemId) ? new GameError('stolen', use === 'sell' ? 'Traders will not buy stolen goods' : 'Stolen goods cannot be traded') : null,
      service: (heroId, service) => this.serviceBlock(heroId, service),
    });
  }

  attach(host: LawHost): void {
    this.host = host;
  }

  // ---------------------------------------------------------------- towns

  townOf(region: string): Jurisdiction | undefined {
    return this.rules.jurisdictions.find((j) => j.regions.includes(region));
  }

  isPrisonRegion(region: string): boolean {
    return region === this.rules.jail.region || this.rules.bargate.grounds.includes(region);
  }

  // ---------------------------------------------------------------- online clock

  onlineMs(heroId: number): number {
    let v = this.online.get(heroId);
    if (v === undefined) {
      v = this.db.get<{ online_ms: number }>('SELECT online_ms FROM law_heroes WHERE hero_id = ?', heroId)?.online_ms ?? 0;
      this.online.set(heroId, v);
    }
    return v;
  }

  /**
   * Advance the online clock of every hero in the world, serve sentences and release
   * whoever has served their time. Called once per server tick.
   */
  tick(dtMs: number, heroIds: Iterable<number>): void {
    const now = this.clock();
    for (const id of heroIds) {
      this.online.set(id, this.onlineMs(id) + dtMs);
      this.dirty.add(id);
      const s = this.openSentence(id);
      if (!s) continue;
      const where = this.host?.regionOf(id);
      if (where !== undefined && !this.isPrisonRegion(where)) {
        // Somehow outside (a region change raced the arrest): put them back.
        this.host?.relocate(id, this.cellFor(s), { private: s.prison === 'jail' });
        continue;
      }
      this.db.run('UPDATE law_sentences SET served_ms = MIN(total_ms, served_ms + ?) WHERE id = ?', dtMs, s.id);
      if (s.served_ms + dtMs >= s.total_ms) this.release(id, 'served');
    }
    for (const [id, until] of this.resisting) if (until <= now) this.resisting.delete(id);
    for (const [k, until] of this.blind) if (until <= now) this.blind.delete(k);
    if ((this.flushIn -= dtMs) <= 0) this.flush();
  }

  /** Write online clocks to the database (every 10 s, and when a hero leaves). */
  flush(heroId?: number): void {
    this.flushIn = 10_000;
    const ids = heroId === undefined ? [...this.dirty] : this.dirty.has(heroId) ? [heroId] : [];
    for (const id of ids) {
      this.ensureRow(id);
      this.db.run('UPDATE law_heroes SET online_ms = ? WHERE hero_id = ?', this.online.get(id) ?? 0, id);
      this.dirty.delete(id);
    }
  }

  // ---------------------------------------------------------------- crimes

  /**
   * Put a crime on a hero's record. Witnessing is the caller's job (GuardDirector checks
   * sight lines; combat reports crimes against heroes directly). Returns the new row.
   */
  record(heroId: number, kind: CrimeKind, ctx: { town: string | null; value?: number; victim?: string }): CrimeRow {
    const def = this.rules.crimes[kind];
    if (!def) throw new GameError('bad_crime', `Unknown crime ${kind}`);
    const now = this.clock();
    this.wantedCache.delete(heroId);
    const repeats = this.db.get<{ n: number }>(
      'SELECT COUNT(*) AS n FROM law_crimes WHERE hero_id = ? AND kind = ? AND created_at > ?',
      heroId,
      kind,
      now - this.rules.repeatWindowMs,
    )!.n;
    const value = Math.max(0, Math.floor(ctx.value ?? 0));
    const base = Math.max(def.minFine, def.fine + def.valueFactor * value);
    const fine = def.payable ? Math.min(this.rules.maxFine, Math.round(base * this.rules.repeatFactor ** repeats)) : 0;
    const { lastInsertRowid: id } = this.db.run(
      'INSERT INTO law_crimes (hero_id, kind, town, fine, level, victim, online_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
      heroId,
      kind,
      ctx.town,
      fine,
      def.level,
      ctx.victim ?? null,
      this.onlineMs(heroId),
      now,
    );
    this.events.emit({ type: 'crime', heroId, kind, town: ctx.town, fine, level: this.wantedLevel(heroId) });
    if (kind === 'murder_player' && this.rules.open.playerBounty && ctx.victim)
      this.events.emit({ type: 'bounty_placed', heroId, victim: ctx.victim });
    this.pushStatus(heroId);
    return this.db.get<CrimeRow>('SELECT * FROM law_crimes WHERE id = ?', id)!;
  }

  /** Unsettled crimes, after forgiving petty ones that have aged out in online time. */
  openCrimes(heroId: number, town?: string): CrimeRow[] {
    const rows = this.db.all<CrimeRow>('SELECT * FROM law_crimes WHERE hero_id = ? AND settled IS NULL ORDER BY id', heroId);
    const clock = this.onlineMs(heroId);
    const forgiveMs = this.rules.pettyForgiveMinutes * MIN;
    const open: CrimeRow[] = [];
    for (const c of rows) {
      if (this.rules.crimes[c.kind]?.petty && clock - c.online_at >= forgiveMs) {
        this.db.run("UPDATE law_crimes SET settled = 'forgiven', settled_at = ? WHERE id = ?", this.clock(), c.id);
        continue;
      }
      if (town === undefined || c.town === town) open.push(c);
    }
    return open;
  }

  /** Wanted level (0..4), across all towns unless `town` is given. Cached for a second. */
  wantedLevel(heroId: number, town?: string): number {
    if (town !== undefined) return this.computeWanted(heroId, town);
    const now = this.clock();
    const hit = this.wantedCache.get(heroId);
    if (hit && now - hit.at < 1000) return hit.level;
    const level = this.computeWanted(heroId);
    this.wantedCache.set(heroId, { level, at: now });
    return level;
  }

  private computeWanted(heroId: number, town?: string): number {
    const open = this.openCrimes(heroId, town);
    let level = 0;
    let total = 0;
    for (const c of open) {
      level = Math.max(level, c.level);
      total += c.fine;
    }
    for (const [gold, l] of this.rules.levelByTotal) if (total >= gold) level = Math.max(level, l);
    return level;
  }

  status(heroId: number): LawStatus {
    const open = this.openCrimes(heroId);
    const lines = new Map<CrimeKind, { kind: CrimeKind; textId: string; count: number; gold: number }>();
    let total = 0;
    let oldestPetty: number | null = null;
    for (const c of open) {
      const l = lines.get(c.kind) ?? { kind: c.kind, textId: this.rules.crimes[c.kind].textId, count: 0, gold: 0 };
      l.count++;
      l.gold += c.fine;
      lines.set(c.kind, l);
      total += c.fine;
      if (this.rules.crimes[c.kind].petty) oldestPetty = Math.min(oldestPetty ?? Infinity, c.online_at);
    }
    const clock = this.onlineMs(heroId);
    const s = this.openSentence(heroId);
    return {
      level: this.wantedLevel(heroId),
      total,
      lines: [...lines.values()],
      forgivenInMinutes: oldestPetty === null ? null : Math.max(0, Math.ceil((oldestPetty + this.rules.pettyForgiveMinutes * MIN - clock) / MIN)),
      sentence: s ? { prison: s.prison, leftMs: s.total_ms - s.served_ms, region: this.cellFor(s) } : null,
      kickedFrom: this.db
        .all<{ town: string; until_online_ms: number }>('SELECT town, until_online_ms FROM law_kickouts WHERE hero_id = ? AND until_online_ms > ?', heroId, clock)
        .map((k) => ({ town: k.town, leftMs: k.until_online_ms - clock })),
    };
  }

  /** Send the current record to the hero's client (it writes it into Fable's crime memory). */
  pushStatus(heroId: number): void {
    this.wantedCache.delete(heroId);
    this.host?.notify(heroId, { t: 'law_status', ...this.status(heroId) });
  }

  // ---------------------------------------------------------------- pursuit

  /** Mark a hero as running from the law (ignored a guard, broke free, attacked a guard). */
  markResisting(heroId: number): void {
    this.resisting.set(heroId, this.clock() + this.rules.guards.loseMs);
  }
  /** Guards still see a resisting hero: keep the chase going. */
  stillSeen(heroId: number): void {
    if (this.resisting.has(heroId)) this.resisting.set(heroId, this.clock() + this.rules.guards.loseMs);
  }
  isResisting(heroId: number): boolean {
    const t = this.resisting.get(heroId);
    return t !== undefined && t > this.clock();
  }
  clearResisting(heroId: number): void {
    this.resisting.delete(heroId);
  }

  /** Whether this town's guards are looking the other way for this hero (a recent bribe). */
  isBlind(heroId: number, town: string): boolean {
    return (this.blind.get(`${heroId}:${town}`) ?? 0) > this.clock();
  }

  // ---------------------------------------------------------------- fines

  /** Gold a guard asks for: every payable open crime, wherever it happened. */
  quoteFine(heroId: number): number {
    return this.openCrimes(heroId).reduce((s, c) => s + (this.rules.crimes[c.kind].payable ? c.fine : 0), 0);
  }

  /**
   * Pay every payable open crime. Level-2 crimes still throw the hero out of the town the
   * guard was in (Fable's "fined and kicked out of the area"). Crimes that can't be paid
   * (a murdered hero) stay on the record.
   */
  payFine(heroId: number, key: string, atTown?: string): { paid: number; kicked: { town: string; minutes: number } | null } {
    const open = this.openCrimes(heroId).filter((c) => this.rules.crimes[c.kind].payable);
    if (open.length === 0) return { paid: 0, kicked: null };
    const paid = open.reduce((s, c) => s + c.fine, 0);
    if (paid > 0 && this.economy.balance(heroId) < paid) throw new GameError('insufficient_gold', 'Not enough gold for the fine');
    let kick = 0;
    for (const c of open) kick = Math.max(kick, c.level >= 2 ? this.rules.crimes[c.kind].kickMinutes : 0);
    this.db.tx(() => {
      const tx = paid > 0 ? this.economy.spend(heroId, paid, 'crime', key).txId : null;
      for (const c of open) this.db.run("UPDATE law_crimes SET settled = 'paid', settled_at = ?, settled_tx = ? WHERE id = ?", this.clock(), tx, c.id);
    });
    this.events.emit({ type: 'fine_paid', heroId, gold: paid, crimes: open.length });
    this.clearResisting(heroId);
    let kicked: { town: string; minutes: number } | null = null;
    if (kick > 0 && atTown) {
      this.kick(heroId, atTown, kick);
      kicked = { town: atTown, minutes: kick };
    }
    this.pushStatus(heroId);
    return { paid, kicked };
  }

  // ---------------------------------------------------------------- bribes

  /**
   * What the town's guards want to look the other way, or null when a bribe can't help
   * (a murder, a crime against a hero, or wanted beyond the bribe limit in that town).
   */
  quoteBribe(heroId: number, town: string): number | null {
    const open = this.openCrimes(heroId, town);
    if (open.length === 0) return null;
    if (open.some((c) => !this.rules.crimes[c.kind].bribable)) return null;
    if (this.wantedLevel(heroId, town) > this.rules.bribe.maxLevel) return null;
    if (this.openCrimes(heroId).some((c) => c.kind === 'murder_player')) return null;
    const fines = open.reduce((s, c) => s + c.fine, 0);
    const b = this.rules.bribe;
    const n = this.daily(heroId).bribes_today;
    return Math.min(this.rules.maxFine * 4, Math.max(b.min, Math.ceil(fines * b.factor)) * 2 ** n);
  }

  /** Pay the bribe: clears this town's crimes, and its guards ignore petty crimes for a while. */
  bribe(heroId: number, town: string, key: string): { paid: number } {
    const price = this.quoteBribe(heroId, town);
    if (price === null) throw new GameError('no_bribe', 'The guards will not take a bribe for that');
    const open = this.openCrimes(heroId, town);
    this.db.tx(() => {
      const { txId, replayed } = this.economy.spend(heroId, price, 'crime', key);
      if (replayed) return;
      for (const c of open) this.db.run("UPDATE law_crimes SET settled = 'bribed', settled_at = ?, settled_tx = ? WHERE id = ?", this.clock(), txId, c.id);
      this.ensureRow(heroId);
      this.db.run('UPDATE law_heroes SET bribes_today = bribes_today + 1 WHERE hero_id = ?', heroId);
    });
    this.blind.set(`${heroId}:${town}`, this.clock() + this.rules.bribe.blindMinutes * MIN);
    this.clearResisting(heroId);
    this.events.emit({ type: 'bribe_paid', heroId, town, gold: price });
    this.pushStatus(heroId);
    return { paid: price };
  }

  // ---------------------------------------------------------------- kick-outs

  kick(heroId: number, town: string, minutes: number): void {
    const until = this.onlineMs(heroId) + minutes * MIN;
    this.db.run(
      'INSERT INTO law_kickouts (hero_id, town, until_online_ms) VALUES (?, ?, ?) ON CONFLICT (hero_id, town) DO UPDATE SET until_online_ms = MAX(until_online_ms, excluded.until_online_ms)',
      heroId,
      town,
      until,
    );
    const j = this.rules.jurisdictions.find((x) => x.id === town);
    const where = this.host?.regionOf(heroId);
    if (j && where && j.regions.includes(where) && this.host!.hasRegion(j.kickTo)) this.host!.relocate(heroId, j.kickTo, { private: false });
    this.events.emit({ type: 'kicked_out', heroId, town, minutes });
  }

  isKickedFrom(heroId: number, town: string): boolean {
    const row = this.db.get<{ until_online_ms: number }>('SELECT until_online_ms FROM law_kickouts WHERE hero_id = ? AND town = ?', heroId, town);
    return !!row && row.until_online_ms > this.onlineMs(heroId);
  }

  // ---------------------------------------------------------------- region rules

  /**
   * May this hero enter `to`? Called by the Director for every region change. A prisoner
   * stays in the prison grounds; a hero thrown out of a town stays out until the time is up.
   */
  checkEnter(heroId: number, to: string, via: string): GameError | null {
    if (via === 'law') return null;
    const s = this.openSentence(heroId);
    if (s) {
      const ok = s.prison === 'bargate' ? this.rules.bargate.grounds.includes(to) && via === 'exit' : false;
      return ok ? null : new GameError('in_prison', 'You are serving a sentence', 403);
    }
    const town = this.townOf(to);
    if (town && this.isKickedFrom(heroId, town.id)) return new GameError('kicked_out', 'You have been thrown out of this town for now', 403);
    if (via === 'cullis_gate') return this.serviceBlock(heroId, 'cullis_gate');
    return null;
  }

  /** Where a hero must appear on login: their cell if they are still serving time. */
  placement(heroId: number): { region: string; private: boolean } | undefined {
    const s = this.openSentence(heroId);
    return s ? { region: this.cellFor(s), private: s.prison === 'jail' } : undefined;
  }

  /** Shops, trading, Cullis Gates and property actions are refused while wanted or in prison. */
  serviceBlock(heroId: number, service: 'shop' | 'trade' | 'cullis_gate' | 'property'): GameError | null {
    if (this.openSentence(heroId)) return new GameError('in_prison', 'You are serving a sentence', 403);
    if (this.wantedLevel(heroId) >= this.rules.servicesBlockedAt) return new GameError('wanted', `You are wanted (${service} refused)`, 403);
    return null;
  }

  // ---------------------------------------------------------------- prison

  openSentence(heroId: number): SentenceRow | undefined {
    return this.db.get<SentenceRow>('SELECT * FROM law_sentences WHERE hero_id = ? AND released_at IS NULL', heroId);
  }

  /**
   * Arrest: settle the open record by a sentence and move the hero to jail or Bargate.
   * Bargate for wanted level 4 (a murdered hero) or the third jail trip in a week.
   */
  sentence(heroId: number, reason: string): SentenceRow {
    const existing = this.openSentence(heroId);
    if (existing) return existing;
    const open = this.openCrimes(heroId);
    const level = this.computeWanted(heroId);
    const fines = open.reduce((s, c) => s + c.fine, 0);
    const now = this.clock();
    const trips = (p: Prison, days: number) =>
      this.db.get<{ n: number }>('SELECT COUNT(*) AS n FROM law_sentences WHERE hero_id = ? AND prison = ? AND created_at > ?', heroId, p, now - days * DAY_MS)!.n;
    const j = this.rules.jail;
    const g = this.rules.bargate;
    const jailTrips = trips('jail', j.repeatWindowDays);
    const prison: Prison = level >= 4 || jailTrips >= j.tripsBeforeBargate ? 'bargate' : 'jail';
    let minutes: number;
    if (prison === 'jail') minutes = clamp((fines / 1000) * j.minutesPer1000, j.minMinutes, j.maxMinutes);
    else minutes = Math.min(g.maxMinutes, g.firstMinutes * g.repeatFactor ** trips('bargate', g.repeatWindowDays));
    const row = this.db.tx(() => {
      const { lastInsertRowid } = this.db.run(
        'INSERT INTO law_sentences (hero_id, prison, total_ms, fines, reason, created_at) VALUES (?, ?, ?, ?, ?, ?)',
        heroId,
        prison,
        Math.round(minutes * MIN),
        fines,
        reason,
        now,
      );
      for (const c of open) this.db.run("UPDATE law_crimes SET settled = 'served', settled_at = ? WHERE id = ?", now, c.id);
      return this.db.get<SentenceRow>('SELECT * FROM law_sentences WHERE id = ?', lastInsertRowid)!;
    });
    this.clearResisting(heroId);
    this.events.emit({ type: 'sentenced', heroId, prison, minutes: Math.round(minutes * 10) / 10, reason, fines });
    if (this.host?.regionOf(heroId) !== undefined) this.host.relocate(heroId, this.cellFor(row), { private: prison === 'jail' });
    this.pushStatus(heroId);
    return row;
  }

  /** Jail only: pay the settled fines plus the surcharge to walk out now. */
  payOut(heroId: number, key: string): { paid: number } {
    const s = this.openSentence(heroId);
    if (!s || s.prison !== 'jail') throw new GameError('no_payout', 'Only a jail sentence can be paid off', 409);
    const price = Math.max(1, Math.ceil(s.fines * (1 + this.rules.jail.earlyReleaseSurcharge)));
    this.economy.spend(heroId, price, 'crime', key);
    this.release(heroId, 'paid');
    return { paid: price };
  }

  /** Bargate only, in the Warden's Office, once per sentence: pay to halve the time left. */
  quoteWarden(heroId: number): number | null {
    const s = this.openSentence(heroId);
    if (!s || s.prison !== 'bargate' || s.warden_used) return null;
    const halvedMin = (s.total_ms - s.served_ms) / 2 / MIN;
    return Math.max(1, Math.ceil(halvedMin * this.rules.bargate.wardenGoldPerMinute));
  }

  warden(heroId: number, key: string): { paid: number; leftMs: number } {
    const price = this.quoteWarden(heroId);
    if (price === null) throw new GameError('no_warden', 'The warden will not deal', 409);
    if (this.host?.regionOf(heroId) !== 'PrisonOffice') throw new GameError('not_here', "Go to the Warden's Office", 409);
    const s = this.openSentence(heroId)!;
    const left = Math.ceil((s.total_ms - s.served_ms) / 2);
    this.db.tx(() => {
      this.economy.spend(heroId, price, 'crime', key);
      this.db.run('UPDATE law_sentences SET warden_used = 1, total_ms = served_ms + ? WHERE id = ?', left, s.id);
    });
    this.events.emit({ type: 'warden_paid', heroId, gold: price });
    this.pushStatus(heroId);
    return { paid: price, leftMs: left };
  }

  /**
   * The Bargate escape (off until it is decided). One try per sentence: making it out takes
   * a share off the time left, getting caught multiplies it.
   */
  escape(heroId: number, made: boolean): { leftMs: number } {
    if (!this.rules.open.bargateEscape) throw new GameError('disabled', 'Escapes are not open yet', 403);
    const s = this.openSentence(heroId);
    if (!s || s.prison !== 'bargate' || s.escape_tried) throw new GameError('no_escape', 'No escape to try', 409);
    const left = s.total_ms - s.served_ms;
    const o = this.rules.open;
    const next = Math.min(this.rules.bargate.maxMinutes * MIN, Math.round(made ? left * (1 - o.escapeReward) : left * o.escapeCaughtFactor));
    this.db.run('UPDATE law_sentences SET escape_tried = 1, total_ms = served_ms + ? WHERE id = ?', Math.max(1, next), s.id);
    this.events.emit({ type: 'escape', heroId, made });
    this.pushStatus(heroId);
    return { leftMs: next };
  }

  private release(heroId: number, how: 'served' | 'paid'): void {
    const s = this.openSentence(heroId);
    if (!s) return;
    this.db.run('UPDATE law_sentences SET released_at = ?, release = ? WHERE id = ?', this.clock(), how, s.id);
    const to = s.prison === 'jail' ? this.rules.jail.releaseTo : this.rules.bargate.releaseTo;
    this.events.emit({ type: 'released', heroId, prison: s.prison, how });
    if (this.host?.regionOf(heroId) !== undefined && this.host.hasRegion(to)) this.host.relocate(heroId, to, { private: false });
    this.pushStatus(heroId);
  }

  private cellFor(s: SentenceRow): string {
    if (s.prison === 'jail') return this.rules.jail.region;
    const g = this.rules.bargate;
    const now = this.clock();
    const trips = this.db.get<{ n: number }>("SELECT COUNT(*) AS n FROM law_sentences WHERE hero_id = ? AND prison = 'bargate' AND created_at > ?", s.hero_id, now - g.repeatWindowDays * DAY_MS)!.n;
    if (trips >= g.tortureAfterTrips && s.served_ms < s.total_ms / 2) return g.tortureRegion;
    const cells = g.cells.filter((c) => !this.host || this.host.hasRegion(c));
    return cells[s.hero_id % Math.max(1, cells.length)] ?? g.cells[0]!;
  }

  // ---------------------------------------------------------------- stealing

  /**
   * A steal attempt (Fable's CTCActionUseBuyOrSteal, sent as a request). The server checks
   * the daily caps and the shop's per-hero stock, then rolls whether anyone noticed. Only
   * an unseen steal gives the item, with a stolen mark; a seen one is a crime and the item
   * stays in the shop. A client that hides being spotted gains nothing: the server rolls.
   */
  steal(
    heroId: number,
    req: {
      shop: string;
      defId: string;
      town: string | null;
      /** NPC shop or home. Player property can never be stolen from. */
      npcOwned: boolean;
      guardInView: boolean;
      isDay: boolean;
      stealthPieces: number;
      clientSpotted: boolean;
      /** The client's pickpocket variant (fine and text differ). */
      pickpocket?: boolean;
    },
    key: string,
  ): { spotted: boolean; itemId: string | null; crime: CrimeRow | null; chance: number } {
    const r = this.rules.stealing;
    if (!req.npcOwned) throw new GameError('not_stealable', 'You can only steal from townsfolk', 403);
    if (this.openSentence(heroId)) throw new GameError('in_prison', 'You are serving a sentence', 403);
    const def = this.economy.catalog.get(req.defId);
    if (def.buy === null || !def.tradeable) throw new GameError('not_stealable', 'That cannot be stolen', 403);
    const value = def.buy;
    const now = this.clock();
    const day = this.daily(heroId);
    if (day.last_steal_at !== null && now - day.last_steal_at < r.cooldownSec * 1000) throw new GameError('too_fast', 'Wait a moment before trying again', 429);
    if (day.stolen_today + value > r.dailyValueCap) throw new GameError('steal_cap', 'You have stolen enough for today', 429);
    const taken = this.db.get<{ n: number }>(
      'SELECT COUNT(*) AS n FROM law_steals WHERE hero_id = ? AND shop = ? AND def_id = ? AND day = ? AND spotted = 0',
      heroId,
      req.shop,
      req.defId,
      day.day,
    )!.n;
    if (taken >= r.stockPerItem) throw new GameError('no_stock', 'There is nothing left to take', 409);
    const recent = this.db.get<{ n: number }>('SELECT COUNT(*) AS n FROM law_steals WHERE hero_id = ? AND created_at > ?', heroId, now - 3600_000)!.n;

    let chance = r.baseSpotChance + recent * r.repeatBonus - req.stealthPieces * r.stealthPerPiece;
    if (req.isDay) chance += r.dayBonus;
    if (req.guardInView) chance += r.guardInViewBonus;
    chance = clamp(chance, r.minChance, r.maxChance);
    if (req.clientSpotted) chance = Math.min(r.maxChance, chance + r.clientSpottedBonus);
    chance = Math.round(chance * 1000) / 1000;
    const spotted = this.rng() < chance;

    let itemId: string | null = null;
    let crime: CrimeRow | null = null;
    this.db.tx(() => {
      this.db.run('UPDATE law_heroes SET last_steal_at = ? WHERE hero_id = ?', now, heroId);
      this.db.run(
        'INSERT INTO law_steals (hero_id, shop, def_id, day, value, spotted, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)',
        heroId,
        req.shop,
        req.defId,
        day.day,
        value,
        spotted ? 1 : 0,
        now,
      );
      if (spotted) {
        crime = this.record(heroId, req.pickpocket ? 'pickpocket' : 'theft', { town: req.town, value, victim: req.shop });
      } else {
        itemId = this.economy.mintItem(heroId, req.defId, 1, 'stolen', key).itemId;
        this.db.run('INSERT INTO law_stolen (item_id, hero_id, value, until) VALUES (?, ?, ?, ?)', itemId, heroId, value, now + r.markHours * 3600_000);
        this.db.run('UPDATE law_heroes SET stolen_today = stolen_today + ? WHERE hero_id = ?', value, heroId);
      }
    });
    this.events.emit({ type: 'steal', heroId, shop: req.shop, defId: req.defId, value, spotted, chance: Math.round(chance * 100) / 100 });
    return { spotted, itemId, crime, chance };
  }

  isMarked(itemId: string): boolean {
    const m = this.db.get<{ until: number }>('SELECT until FROM law_stolen WHERE item_id = ?', itemId);
    return !!m && m.until > this.clock();
  }

  /** The black marketeer: the only buyer of marked goods, at a quarter of their value. */
  fence(heroId: number, itemId: string, key: string): { gold: number } {
    const m = this.db.get<{ value: number; until: number }>('SELECT value, until FROM law_stolen WHERE item_id = ?', itemId);
    if (!m || m.until <= this.clock()) throw new GameError('not_stolen', 'The fence only buys hot goods', 409);
    const day = this.daily(heroId);
    if (day.fenced_today + m.value > this.rules.stealing.fenceDailyCap) throw new GameError('fence_cap', 'The fence has bought enough from you today', 429);
    let gold = 0;
    this.db.tx(() => {
      const r = this.economy.sellAtRatio(heroId, itemId, this.rules.stealing.fenceRatio, key);
      gold = r.gold;
      if (!r.replayed) this.db.run('UPDATE law_heroes SET fenced_today = fenced_today + ? WHERE hero_id = ?', m.value, heroId);
    });
    return { gold };
  }

  // ---------------------------------------------------------------- internals

  private ensureRow(heroId: number): void {
    this.db.run('INSERT OR IGNORE INTO law_heroes (hero_id) VALUES (?)', heroId);
  }

  /** The hero's daily counters, reset when the UTC day changes. */
  private daily(heroId: number): { day: string; bribes_today: number; stolen_today: number; fenced_today: number; last_steal_at: number | null } {
    this.ensureRow(heroId);
    const today = new Date(this.clock()).toISOString().slice(0, 10);
    const row = this.db.get<{ day: string | null; bribes_today: number; stolen_today: number; fenced_today: number; last_steal_at: number | null }>(
      'SELECT day, bribes_today, stolen_today, fenced_today, last_steal_at FROM law_heroes WHERE hero_id = ?',
      heroId,
    )!;
    if (row.day !== today) {
      this.db.run('UPDATE law_heroes SET day = ?, bribes_today = 0, stolen_today = 0, fenced_today = 0 WHERE hero_id = ?', today, heroId);
      return { day: today, bribes_today: 0, stolen_today: 0, fenced_today: 0, last_steal_at: row.last_steal_at };
    }
    return { ...row, day: today };
  }

  /** A fresh idempotency key for server-initiated actions (auto-fines at a kick-out). */
  static key(prefix: string): string {
    return `${prefix}-${crypto.randomUUID()}`;
  }
}

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

export { TEXT };
