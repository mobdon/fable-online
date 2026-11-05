import type { NavMesh } from '../world/nav/navmesh.js';
import type { Creature, Combatant } from '../world/ai/types.js';
import type { AiDirector } from '../world/ai/brain.js';
import { GUARD_DEF } from '../world/ai/profiles.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';
import { Law } from './law.js';
import { BARKS, TEXT, barkId, type CrimeKind, type Jurisdiction } from './rules.js';
import { inAnyZone, type TownZone } from '../world/town/zones.js';
import { shopIdOf } from '../world/town/townsfolk.js';
import { SHOP_REACH, type ShopDef } from '../economy/shops.js';

/** Villagers and traders that can witness crimes and report them to a guard. */
export const VILLAGER_DEF = /VILLAGER|TRADER|SHOPKEEPER|BARMAN|CHILD|_WIFE|_HUSBAND|FARMER/;
/** Creature factions the law protects: hurting them is assault, killing them murder. */
export const PROTECTED_FACTIONS = new Set(['villager', 'guard', 'trader']);

export interface LawHeroView {
  id: number;
  pos: { x: number; y: number; z: number };
  alive: boolean;
}

/** The slice of a channel the guards work in. */
export interface LawChannel {
  channelId: string;
  region: string;
  nav: NavMesh | undefined;
  creatures(): Iterable<Creature>;
  heroes(): Iterable<LawHeroView>;
  ai: Pick<AiDirector, 'pursue' | 'approach' | 'standDown' | 'state'>;
  /** Send a law frame to one hero. */
  send(heroId: number, msg: Record<string, unknown>): void;
  /** Send a law frame to everyone in the channel (wanted markers, barks). */
  broadcast(msg: Record<string, unknown>): void;
}

/** Facts the mod reports and the server can't see itself, plus hooks for other systems. */
export interface LawServices {
  law: Law;
  events: EventSink;
  /** Is it daytime in Albion? (Day and night sync is separate work; daytime is the safe default.) */
  isDay?: () => boolean;
  /** Stealth armour pieces the hero has equipped, from the server's own records. */
  stealthPieces?: (heroId: number) => number;
  /** Shops and homes that belong to heroes can never be stolen from. */
  playerOwned?: (shopId: string) => boolean;
  /** The server's shops by id (the app wires this to Shops.get). Without it nothing can be stolen. */
  shop?: (shopId: string) => ShopDef | undefined;
  rng?: () => number;
  /** Where law frames go (the app wires this to the Director). */
  out?: { send(heroId: number, msg: Record<string, unknown>): void; broadcast(channelId: string, msg: Record<string, unknown>): void };
}

type Stage = 'warn' | 'ask' | 'prompt' | 'pursue';

interface Case {
  heroId: number;
  guardId: number;
  stage: Stage;
  since: number;
  /** warn: warnings given so far, and what the warning is about. */
  warnings: number;
  warnFor: 'weapon' | 'trespass' | null;
  promptAt: number;
  /** pursue: how long a guard has been within arrest range. */
  holdMs: number;
  lostAt: number | null;
}

const THINK_MS = 250;
const dist = (a: { x: number; y: number }, b: { x: number; y: number }) => Math.hypot(a.x - b.x, a.y - b.y);

/**
 * The town watch for one channel. Guards are ordinary server-run creatures (the monster
 * AI moves and fights them); this decides what they want:
 *
 *  - a crime in a guard's sight goes on the record at once, and a bark plays;
 *  - a villager who saw it tells the guards a few seconds later;
 *  - wanted level 1: a guard walks up and asks for the fine (or a bribe);
 *  - level 2: the same, and paying throws the hero out of the town for a while;
 *  - level 3+, or a hero who walked away, refused or hit a guard: every guard that sees
 *    them gives chase, and one staying in arm's reach for a moment makes the arrest;
 *  - a weapon out in town gets three warnings before it is a crime.
 */
export class ChannelLaw {
  readonly town: Jurisdiction | undefined;
  private cases = new Map<number, Case>();
  private weaponOut = new Set<number>();
  private trespassing = new Map<number, string>();
  private reports: { at: number; heroId: number; kind: CrimeKind; value?: number; victim?: string; villagerId: number }[] = [];
  /** `${hero}:${victim}` -> damage dealt this fight, for assault / aggravated assault. */
  private harm = new Map<string, { dmg: number; at: number; gbh: boolean }>();
  private now = 0;
  private nextThink = 0;
  private lastLevel = new Map<number, number>();
  private rng: () => number;
  private witnessSources: (() => Iterable<Creature>)[] = [];
  private zones: TownZone[] = [];

  constructor(
    private ch: LawChannel,
    private s: LawServices,
  ) {
    this.town = s.law.townOf(ch.region);
    this.rng = s.rng ?? Math.random;
  }

  private get rules() {
    return this.s.law.rules;
  }

  // ---------------------------------------------------------------- AI hook

  /** AiWorld.isHostile for guards: only criminals the law says to chase. */
  guardHostile(guard: Creature, t: Combatant): boolean {
    if (!GUARD_DEF.test(guard.defName)) return t.faction !== guard.faction;
    if (t.faction !== 'hero' || !t.alive) return false;
    const c = this.cases.get(t.id);
    return !!c && c.stage === 'pursue';
  }

  /** The region's towns (from the level files). A drawn weapon outside them is nobody's business. */
  setZones(zones: TownZone[]): void {
    this.zones = zones;
  }

  /** In town, or near enough for the watch to care. Without zones the whole region counts. */
  private inTown(p: { x: number; y: number }): boolean {
    return !this.zones.length || inAnyZone(p, this.zones, 5);
  }

  // ---------------------------------------------------------------- reports from the mod

  /** The hero drew or sheathed a weapon. */
  setWeaponOut(heroId: number, on: boolean): void {
    if (on) this.weaponOut.add(heroId);
    else this.weaponOut.delete(heroId);
  }

  /** The hero walked into (zone) or out of (null) private property. */
  setTrespass(heroId: number, zone: string | null): void {
    if (zone) this.trespassing.set(heroId, zone);
    else this.trespassing.delete(heroId);
  }

  /** Lock picked, or something smashed. Recorded only if someone saw it. */
  report(heroId: number, kind: 'lockpicking' | 'vandalism', value = 0): void {
    this.witnessed(heroId, kind, { value });
  }

  /**
   * A steal request. `shop` is the shop's uid in this region, as in shop_open/shop_buy. The
   * shop must be one the server knows in this region, stock the item, and the hero must stand
   * within its reach. The guards' sight lines feed the spotted roll.
   */
  steal(heroId: number, shop: string, defId: string, opts: { pickpocket?: boolean; clientSpotted?: boolean }, key: string) {
    const h = this.hero(heroId);
    if (!h || !h.alive) throw new GameError('offline', 'Not in the world');
    const shopId = shopIdOf(this.ch.region, shop);
    const s = this.s.shop?.(shopId);
    if (!s || s.region !== this.ch.region) throw new GameError('no_shop', 'No such shop', 404);
    if (!s.stock.includes(defId)) throw new GameError('not_stocked', 'This shop does not sell that');
    if (s.pos && dist(h.pos, s.pos) > (s.reach ?? SHOP_REACH)) throw new GameError('too_far', 'You are not at that shop');
    const guard = this.seenBy(h.pos, 'guard');
    const r = this.s.law.steal(
      heroId,
      {
        shop: shopId,
        defId,
        town: this.town?.id ?? null,
        npcOwned: !(this.s.playerOwned?.(shopId) ?? false),
        guardInView: !!guard,
        isDay: this.s.isDay?.() ?? true,
        stealthPieces: this.s.stealthPieces?.(heroId) ?? 0,
        clientSpotted: !!opts.clientSpotted,
        ...(opts.pickpocket ? { pickpocket: true } : {}),
      },
      key,
    );
    if (r.spotted) {
      const g = guard ?? this.nearestGuard(h.pos);
      if (g) {
        this.bark(g.id, BARKS[this.rules.crimes[r.crime!.kind].severity]);
        this.openCase(heroId, g.id);
      }
    }
    return r;
  }

  /** The hero's answer to a guard's fine prompt. */
  answer(heroId: number, choice: 'pay' | 'bribe' | 'refuse', key: string): void {
    const c = this.cases.get(heroId);
    if (!c || c.stage !== 'prompt') throw new GameError('no_prompt', 'Nobody is asking you anything', 409);
    const law = this.s.law;
    if (choice === 'refuse') return this.flee(c, BARKS.ranFromWarning);
    if (choice === 'bribe') {
      if (!this.town) throw new GameError('no_bribe', 'No guards to bribe here', 409);
      try {
        law.bribe(heroId, this.town.id, key);
      } catch (e) {
        if (e instanceof GameError && e.code === 'insufficient_gold') {
          this.ch.send(heroId, { t: 'law_notice', text: TEXT.bribeLackFunds });
          return;
        }
        throw e;
      }
      this.bark(c.guardId, BARKS.bribeReceived);
      return this.closeCase(c);
    }
    const fine = law.quoteFine(heroId);
    let paid;
    try {
      paid = law.payFine(heroId, key, this.town?.id);
    } catch (e) {
      if (e instanceof GameError && e.code === 'insufficient_gold') {
        // Fable: a fine the hero can't pay means a night in the cells.
        this.ch.send(heroId, { t: 'law_notice', text: TEXT.lackFunds, gold: fine });
        return this.arrest(heroId, c.guardId, 'unpaid_fine');
      }
      throw e;
    }
    this.bark(c.guardId, BARKS.fineReceived);
    if (paid.kicked) this.ch.send(heroId, { t: 'law_notice', text: TEXT.kickedOut, gold: paid.paid, minutes: paid.kicked.minutes });
    this.closeCase(c);
  }

  // ---------------------------------------------------------------- reports from combat

  /**
   * Combat reports every landed hit. Hits on townsfolk and guards are assault, enough
   * damage to one victim is aggravated assault, a kill is murder. Hits on heroes who
   * haven't flagged for PvP are crimes the server always sees.
   */
  onHarm(attackerId: number, victim: { id: number; kind: 'hero' | 'creature'; faction: string; defName?: string; maxHealth: number; pvp?: boolean }, damage: number, killed: boolean): void {
    if (this.hero(attackerId) === undefined) return; // only heroes commit crimes
    const k = `${attackerId}:${victim.id}`;
    const law = this.s.law;
    if (victim.kind === 'hero') {
      if (victim.pvp !== false) return; // a fair PvP fight
      if (killed) law.record(attackerId, 'murder_player', { town: null, victim: `hero:${victim.id}` });
      else if (!this.harm.has(k)) law.record(attackerId, 'assault', { town: null, victim: `hero:${victim.id}` });
      this.harm.set(k, { dmg: 0, at: this.now, gbh: false });
      this.escalate(attackerId);
      return;
    }
    if (!PROTECTED_FACTIONS.has(victim.faction)) return;
    const isGuard = GUARD_DEF.test(victim.defName ?? '');
    const h = this.harm.get(k) ?? { dmg: 0, at: this.now, gbh: false };
    const first = !this.harm.has(k);
    h.dmg += damage;
    h.at = this.now;
    this.harm.set(k, h);
    let kind: CrimeKind | null = null;
    if (killed) kind = 'murder';
    else if (!h.gbh && h.dmg >= victim.maxHealth * 0.5) {
      h.gbh = true;
      kind = 'gbh';
    } else if (first) kind = 'assault';
    if (kind) this.witnessed(attackerId, kind, { victim: victim.defName ?? 'npc', ...(isGuard && !killed ? { guardId: victim.id } : {}) });
    if (isGuard) {
      // Hitting the watch is resisting arrest, seen or not by anyone else.
      law.markResisting(attackerId);
      this.startPursuit(attackerId, killed ? null : victim.id);
    }
  }

  /** A hero fell to a guard: arrested instead of the usual death. Returns true if so. */
  onHeroDowned(heroId: number, killerId: number | null): boolean {
    const killer = killerId === null ? undefined : this.creature(killerId);
    if (!killer || !GUARD_DEF.test(killer.defName)) return false;
    this.arrest(heroId, killer.id, 'subdued');
    return true;
  }

  heroLeft(heroId: number): void {
    const c = this.cases.get(heroId);
    if (c) this.closeCase(c, false);
    this.weaponOut.delete(heroId);
    this.trespassing.delete(heroId);
    this.lastLevel.delete(heroId);
  }

  // ---------------------------------------------------------------- tick

  step(dtMs: number): void {
    this.now += dtMs;
    for (const c of this.cases.values()) if (c.stage === 'pursue') this.pursueStep(c, dtMs);
    for (let i = this.reports.length - 1; i >= 0; i--) {
      const r = this.reports[i]!;
      if (this.now < r.at) continue;
      this.reports.splice(i, 1);
      const v = this.creature(r.villagerId);
      if (!v || !v.alive) continue; // the witness didn't make it
      this.s.law.record(r.heroId, r.kind, { town: this.town?.id ?? null, ...(r.value ? { value: r.value } : {}), ...(r.victim ? { victim: r.victim } : {}) });
      this.s.events.emit({ type: 'crime_witnessed', heroId: r.heroId, kind: r.kind, witness: 'villager', channelId: this.ch.channelId });
      const g = this.nearestGuard(v.pos);
      if (g) this.openCase(r.heroId, g.id);
    }
    if (this.now < this.nextThink) return;
    this.nextThink = this.now + THINK_MS;
    this.think();
  }

  private think(): void {
    const law = this.s.law;
    for (const h of this.ch.heroes()) {
      if (!h.alive) continue;
      const level = law.wantedLevel(h.id);
      if (this.lastLevel.get(h.id) !== level) {
        this.lastLevel.set(h.id, level);
        this.ch.broadcast({ t: 'law_wanted', hero: h.id, level });
      }
      if (!this.town) continue; // no town watch out here
      const c = this.cases.get(h.id);
      if (c) {
        this.caseStep(c, h, level);
        continue;
      }
      const g = this.seenBy(h.pos, 'guard');
      if (!g) continue;
      if (law.isResisting(h.id) || level >= 3) {
        this.bark(g.id, BARKS.criminal);
        this.openCase(h.id, g.id);
      } else if (level >= 1 && !this.ignoring(h.id)) this.openCase(h.id, g.id);
      else if (this.weaponOut.has(h.id) && this.inTown(h.pos) && !law.isBlind(h.id, this.town.id)) this.warn(h.id, g.id, 'weapon');
      else if (this.trespassing.has(h.id) && !law.isBlind(h.id, this.town.id)) this.warn(h.id, g.id, 'trespass');
    }
  }

  private caseStep(c: Case, h: LawHeroView, level: number): void {
    const g = this.creature(c.guardId);
    const rules = this.rules.guards;
    if (!g || !g.alive) {
      // Guard down: another guard who sees the hero picks the case up, or it lapses.
      const other = this.seenBy(h.pos, 'guard');
      if (other) c.guardId = other.id;
      else if (c.stage !== 'pursue') return this.closeCase(c, false);
      return;
    }
    switch (c.stage) {
      case 'warn': {
        const still = c.warnFor === 'weapon' ? this.weaponOut.has(h.id) && this.inTown(h.pos) : this.trespassing.has(h.id);
        if (!still) {
          this.bark(g.id, BARKS.heeded);
          return this.closeCase(c);
        }
        if (this.now - c.since < rules.warningGapMs) return;
        c.since = this.now;
        if (c.warnings < 3) {
          this.bark(g.id, (c.warnFor === 'weapon' ? BARKS.weapon : BARKS.trespass)[c.warnings]!);
          c.warnings++;
          return;
        }
        // Three warnings ignored: now it's a crime.
        this.bark(g.id, BARKS.ignoredWarning);
        this.s.law.record(h.id, c.warnFor === 'weapon' ? 'weapon_out' : 'trespass', { town: this.town!.id });
        this.weaponOut.delete(h.id);
        this.trespassing.delete(h.id);
        c.stage = 'ask';
        this.ch.ai.approach(g.id, h.id, rules.talkRange);
        return;
      }
      case 'ask': {
        if (level === 0) return this.closeCase(c);
        if (level >= 3 || this.s.law.isResisting(h.id)) return this.startPursuit(h.id, g.id);
        this.ch.ai.approach(g.id, h.id, rules.talkRange);
        if (dist(g.pos, h.pos) > rules.talkRange + 0.3) {
          if (this.now - c.since > rules.answerMs * 2) this.flee(c, BARKS.ranFromWarning);
          return;
        }
        const fine = this.s.law.quoteFine(h.id);
        const bribe = this.s.law.quoteBribe(h.id, this.town!.id);
        c.stage = 'prompt';
        c.promptAt = this.now;
        const worst = this.worstSeverity(h.id);
        this.bark(g.id, BARKS[worst]);
        this.ch.send(h.id, {
          t: 'law_prompt',
          kind: level >= 2 ? 'fine_kick' : 'fine',
          guard: g.id,
          text: TEXT.handOver,
          fine,
          bribe,
          bribeText: TEXT.bribeOffer,
          answerMs: rules.answerMs,
        });
        return;
      }
      case 'prompt': {
        if (level === 0) return this.closeCase(c);
        if (dist(g.pos, h.pos) > rules.walkAwayRange) return this.flee(c, BARKS.ranFromWarning);
        if (this.now - c.promptAt > rules.answerMs) return this.flee(c, BARKS.ignoredWarning);
        return;
      }
      case 'pursue':
        return;
    }
  }

  private pursueStep(c: Case, dtMs: number): void {
    const h = this.hero(c.heroId);
    if (!h || !h.alive) return;
    const law = this.s.law;
    const rules = this.rules.guards;
    const seen = this.seenBy(h.pos, 'guard');
    if (seen) {
      law.stillSeen(h.id);
      c.lostAt = null;
      // Every guard that can see the criminal joins in.
      for (const g of this.guards()) if (g.alive && this.canSee(g, h.pos)) this.ch.ai.pursue(g.id, h.id);
    } else if (c.lostAt === null) {
      c.lostAt = this.now;
      this.bark(c.guardId, BARKS.sweep);
    }
    let close = false;
    for (const g of this.guards()) if (g.alive && dist(g.pos, h.pos) <= rules.arrestRange) close = true;
    c.holdMs = close ? c.holdMs + dtMs : 0;
    if (c.holdMs >= rules.arrestHoldMs) return this.arrest(h.id, c.guardId, 'caught');
    if (c.lostAt !== null && this.now - c.lostAt > rules.loseMs) {
      // Got away for now. The record stays; the next guard to see them starts again.
      this.closeCase(c);
    }
  }

  // ---------------------------------------------------------------- cases

  private openCase(heroId: number, guardId: number): void {
    const existing = this.cases.get(heroId);
    if (existing) return;
    const law = this.s.law;
    const c: Case = { heroId, guardId, stage: 'ask', since: this.now, warnings: 0, warnFor: null, promptAt: 0, holdMs: 0, lostAt: null };
    this.cases.set(heroId, c);
    if (law.isResisting(heroId) || law.wantedLevel(heroId) >= 3) this.startPursuit(heroId, guardId);
    else this.ch.ai.approach(guardId, heroId, this.rules.guards.talkRange);
  }

  private warn(heroId: number, guardId: number, about: 'weapon' | 'trespass'): void {
    // The first warning plays at once; the next ones warningGapMs apart.
    const c: Case = {
      heroId,
      guardId,
      stage: 'warn',
      since: this.now - this.rules.guards.warningGapMs,
      warnings: 0,
      warnFor: about,
      promptAt: 0,
      holdMs: 0,
      lostAt: null,
    };
    this.cases.set(heroId, c);
    this.ch.ai.approach(guardId, heroId, this.rules.guards.talkRange + 1.5);
  }

  private flee(c: Case, bark: string): void {
    this.bark(c.guardId, bark);
    this.s.law.markResisting(c.heroId);
    this.startPursuit(c.heroId, c.guardId);
  }

  private startPursuit(heroId: number, guardId: number | null): void {
    let c = this.cases.get(heroId);
    const gid = guardId ?? this.nearestGuardId(heroId);
    if (gid === null) return;
    if (!c) {
      c = { heroId, guardId: gid, stage: 'pursue', since: this.now, warnings: 0, warnFor: null, promptAt: 0, holdMs: 0, lostAt: null };
      this.cases.set(heroId, c);
    }
    c.stage = 'pursue';
    c.guardId = gid;
    c.lostAt = null;
    this.ch.ai.pursue(gid, heroId);
  }

  private closeCase(c: Case, standDown = true): void {
    this.cases.delete(c.heroId);
    if (!standDown) return;
    for (const g of this.guards()) if (g.targetId === c.heroId) this.ch.ai.standDown(g.id);
  }

  private arrest(heroId: number, guardId: number | null, reason: string): void {
    const c = this.cases.get(heroId);
    if (c) this.closeCase(c);
    this.s.events.emit({ type: 'arrest', heroId, guardId, channelId: this.ch.channelId, reason });
    this.weaponOut.delete(heroId);
    this.trespassing.delete(heroId);
    this.s.law.sentence(heroId, reason);
  }

  /** A bribe makes this town's guards ignore petty crimes for a while. */
  private ignoring(heroId: number): boolean {
    if (!this.town || !this.s.law.isBlind(heroId, this.town.id)) return false;
    return this.s.law.openCrimes(heroId).every((c) => this.rules.crimes[c.kind].petty);
  }

  /** A crime happened: record it if a guard saw it now, or queue a villager's report. */
  private witnessed(heroId: number, kind: CrimeKind, more: { value?: number; victim?: string; guardId?: number }): void {
    const def = this.rules.crimes[kind];
    const h = this.hero(heroId);
    if (!h) return;
    const ctx = { town: this.town?.id ?? null, ...(more.value ? { value: more.value } : {}), ...(more.victim ? { victim: more.victim } : {}) };
    if (!def.needsWitness) {
      this.s.law.record(heroId, kind, ctx);
      return;
    }
    if (this.town && def.petty && this.s.law.isBlind(heroId, this.town.id)) return;
    const guard = (more.guardId !== undefined ? this.creature(more.guardId) : undefined) ?? this.seenBy(h.pos, 'guard');
    if (guard) {
      this.s.law.record(heroId, kind, ctx);
      this.s.events.emit({ type: 'crime_witnessed', heroId, kind, witness: 'guard', channelId: this.ch.channelId });
      this.bark(guard.id, BARKS[def.severity]);
      this.escalate(heroId, guard.id);
      return;
    }
    const v = this.seenBy(h.pos, 'villager');
    if (v) this.reports.push({ at: this.now + this.rules.guards.villagerReportMs, heroId, kind, villagerId: v.id, ...(more.value ? { value: more.value } : {}), ...(more.victim ? { victim: more.victim } : {}) });
  }

  /** After a new crime: start or upgrade the case. */
  private escalate(heroId: number, guardId?: number): void {
    const level = this.s.law.wantedLevel(heroId);
    const c = this.cases.get(heroId);
    if (level >= 3 || this.s.law.isResisting(heroId)) return this.startPursuit(heroId, guardId ?? c?.guardId ?? null);
    if (!c && guardId !== undefined) this.openCase(heroId, guardId);
  }

  private worstSeverity(heroId: number): 'minor' | 'moderate' | 'serious' {
    const order = { minor: 0, moderate: 1, serious: 2 } as const;
    let w: 'minor' | 'moderate' | 'serious' = 'minor';
    for (const c of this.s.law.openCrimes(heroId)) {
      const s = this.rules.crimes[c.kind].severity;
      if (order[s] > order[w]) w = s;
    }
    return w;
  }

  // ---------------------------------------------------------------- sight

  /**
   * Add people who can witness crimes but aren't combat creatures (the town layer's
   * villagers and shopkeepers). They are matched by def name like creatures.
   */
  addWitnessSource(fn: () => Iterable<Creature>): void {
    this.witnessSources.push(fn);
  }

  private *everyone(): Iterable<Creature> {
    yield* this.ch.creatures();
    for (const fn of this.witnessSources) yield* fn();
  }

  private *guards(): Iterable<Creature> {
    for (const c of this.ch.creatures()) if (GUARD_DEF.test(c.defName)) yield c;
  }

  private canSee(c: Creature, p: { x: number; y: number }): boolean {
    const g = this.rules.guards;
    const d = dist(c.pos, p);
    if (d > g.sight) return false;
    if (d > 3) {
      const yaw = Math.atan2(-(p.x - c.pos.x), p.y - c.pos.y);
      const off = Math.abs(Math.atan2(Math.sin(yaw - c.yaw), Math.cos(yaw - c.yaw)));
      if (off > ((g.fovDeg / 2) * Math.PI) / 180) return false;
    }
    return !this.ch.nav || this.ch.nav.clearLine(c.pos, p);
  }

  /** The nearest guard or villager with a clear view of `p`. */
  private seenBy(p: { x: number; y: number }, who: 'guard' | 'villager'): Creature | undefined {
    let best: Creature | undefined;
    let bestD = Infinity;
    for (const c of this.everyone()) {
      if (!c.alive) continue;
      const isGuard = GUARD_DEF.test(c.defName);
      if (who === 'guard' ? !isGuard : isGuard || !VILLAGER_DEF.test(c.defName)) continue;
      const d = dist(c.pos, p);
      if (d < bestD && this.canSee(c, p)) {
        best = c;
        bestD = d;
      }
    }
    return best;
  }

  private nearestGuard(p: { x: number; y: number }): Creature | undefined {
    let best: Creature | undefined;
    let bestD = Infinity;
    for (const g of this.guards()) {
      if (!g.alive) continue;
      const d = dist(g.pos, p);
      if (d < bestD) {
        best = g;
        bestD = d;
      }
    }
    return best;
  }

  private nearestGuardId(heroId: number): number | null {
    const h = this.hero(heroId);
    return (h && this.nearestGuard(h.pos)?.id) ?? null;
  }

  private hero(id: number): LawHeroView | undefined {
    for (const h of this.ch.heroes()) if (h.id === id) return h;
    return undefined;
  }

  private creature(id: number): Creature | undefined {
    for (const c of this.everyone()) if (c.id === id) return c;
    return undefined;
  }

  private bark(guardId: number, family: string): void {
    this.ch.broadcast({ t: 'law_bark', guard: guardId, text: barkId(family, guardId, this.rng()) });
  }
}

export { Law };
