import type { EventSink } from '../analytics/events.js';
import { CombatSystem, defaultHarmPolicy, type DamageStyle, type DeathInfo, type HarmPolicy } from '../combat/combat.js';
import { heroCombatant, type HeroLoadout } from '../combat/loadout.js';
import { BALANCE, type SpellId } from '../combat/rules.js';
import { fableHash } from '../data/fable-bin.js';
import type { GameData } from '../data/game-data.js';
import type { Economy } from '../economy/economy.js';
import type { LootService } from '../economy/loot.js';
import { GameError } from '../errors.js';
import type { Progress } from '../heroes/progress.js';
import type { AiHooks, AttackKind, Creature, Hero } from './entities.js';
import type { RegionDef } from './regions.js';
import type { EntityState, PlayerInput, RegionSim, SimFactory, SimHealing, Vec3 } from './sim.js';
import { HOSTILE_FACTIONS, SpawnManager, allocCreatureId, type SpawnOptions, type SpawnsFile } from './spawns.js';
import { creatureFromDef } from '../combat/loadout.js';
import { AiDirector } from './ai/brain.js';
import { Anim, type AiWorld, type Creature as AiCreature } from './ai/types.js';
import { ChannelLaw, type LawServices } from '../law/guards.js';
import type { NavMesh } from './nav/navmesh.js';
import { inAnyZone, type TownZone } from './town/zones.js';

/** Input button bits the mod sends. Spell slot = (buttons >> SPELL_SHIFT) & 0xF, 1-based. */
export const BUTTONS = { ATTACK: 1 << 0, BLOCK: 1 << 1, CAST: 1 << 2 } as const;
export const SPELL_SHIFT = 8;
export const SPELL_SLOTS: SpellId[] = ['fireball', 'lightning', 'drain_life', 'force_push', 'enflame', 'heal_life', 'physical_shield'];

export const RESURRECTION_PHIALS = ['OBJECT_RESURRECTION_PHIAL', 'ResurrectionPhial'];

export const DEATH = {
  /** Wait before coming back after a death without a phial. */
  respawnMs: 10_000,
  /** Wait before a phial brings the hero back where they fell. */
  phialMs: 2_000,
  /** Share of carried gold lost on a death without a phial, and the most it can be. */
  goldPenalty: 0.05,
  goldPenaltyMax: 5_000,
};

/** Monster AI plugs in here (separate work). It gets the channel's combat and creatures. */
export interface ChannelAi extends AiHooks {
  step?(dtMs: number): void;
  /** Send a creature straight at a hero (a staff test spawn). */
  pursue?(creatureId: number, heroId: number): void;
  dispose?(): void;
}
export interface AiContext {
  region: RegionDef;
  channelId: string;
  combat: CombatSystem;
  creatures: () => Iterable<Creature>;
  heroes: () => Iterable<Hero>;
  /** The town watch: monsters fight back when a guard takes them on. */
  guards?: () => Iterable<Creature>;
}

export interface WorldServices {
  data: GameData;
  spawns: SpawnsFile;
  events: EventSink;
  economy?: Economy;
  loot?: LootService;
  progress?: Progress;
  /** Hero experience (src/heroes/experience.ts): kills pay into it. */
  experience?: { grantKill(heroId: number, xp: number, style: DamageStyle, generalShare: number, detail: string): void };
  /** What the hero has equipped and learned, from the server's own records. */
  loadout?: (heroId: number) => HeroLoadout;
  harm?: HarmPolicy;
  ai?: (ctx: AiContext) => ChannelAi;
  spawnOptions?: Partial<SpawnOptions>;
  /** Crime and the town watch. Without it there are no guards and nothing is a crime. */
  law?: LawServices;
  /**
   * A hero who died without a phial gets back up here: move them to their respawn point
   * (the Cullis Gate nearest `fell`, where they died: src/world/travel/travel.ts). Called
   * after the channel's step, never during it. Returns false when it could not move them:
   * they are put back at their channel entry point and `moved` is called.
   */
  respawn?: (heroId: number, fell: Vec3) => boolean;
  /** The server moved a hero inside the channel on its own (not a region change): tell the client. */
  moved?: (heroId: number, at: Vec3) => void;
  /** Walkable ground for the guards (the region's navmesh); without it they walk straight lines. */
  nav?: (region: string) => NavMesh | undefined;
}

/** Guard stats when game.bin has no entry for the def (test data, or an unread def). */
const GUARD_FALLBACK = { health: 300, power: 1.2, armour: 20 };
/** A dead guard is replaced at its post after this long. */
export const GUARD_RESPAWN_MS = 60_000;

export const TOWN_WATCH = {
  /** A monster this close to a town's outline counts as in town: the watch goes for it. */
  zoneMargin: 10,
  /** Guards this far from a monster in town come running (further than they can see). */
  respondRadius: 45,
  /** Most guards sent after one monster; the rest keep their beat. */
  perMonster: 2,
  thinkMs: 500,
  /**
   * A monster the watch did most of the killing on pays nothing: heroes must deal at least
   * this share of its damage. Stops leading monsters into town for the guards to kill.
   */
  heroShareForLoot: 0.5,
};

interface HeroSlot {
  hero: Hero;
  move: [number, number];
  buttons: number;
  home: Vec3;
  /** ms until respawn while dead, null while alive. */
  downMs: number | null;
  phial: boolean;
  /** A swing or a flinch shown to other players for this many more ms (0: none). */
  actMs: number;
  actAnim: number;
  /** A swing pressed just before the last one finished, fired when it does. */
  queued: AttackKind | null;
  /** The held press is dropped if it has not fired within this many ms (a longer stun came). */
  queuedMs: number;
  /** ms since the hero last dealt or took a hit (sim time), for health regeneration. */
  calmMs: number;
}

/** Health comes back slowly out of combat: after calmMs without a hit, sharePerSec of max per second. */
export const HEALTH_REGEN = { calmMs: 10_000, sharePerSec: 0.02 };

/** A hero's health and Will as shares of their maxima, kept from one channel to the next. */
export interface CarriedVitals {
  health: number;
  will: number;
  at: number;
}
/** A record older than this is dropped: a hero back after a long break starts fresh. */
export const CARRY_EXPIRE_MS = 30 * 60_000;
/** Health share a hero who left while down comes back with (the same as a respawn). */
const RESPAWN_HEALTH_SHARE = 0.5;

/** How long other players see a hero's swing and flinch. */
const SWING_SHOW_MS = 450;
const FLINCH_SHOW_MS = 350;

/**
 * A channel's world: hero movement (same kinematics as StubSim), server-side combat,
 * spawns, loot and deaths. Clients only send stick, camera and button bits.
 */
export class CombatSim implements RegionSim {
  static readonly RUN_SPEED = 6;
  readonly combat: CombatSystem;
  readonly spawns: SpawnManager;
  private heroes = new Map<number, HeroSlot>();
  private ai: ChannelAi | undefined;
  private charging = new Set<number>();
  /** heroId -> when they last dealt or took a hit */
  private lastHit = new Map<number, number>();
  /** The town watch: guard creatures, run by their own copy of the server's AI. */
  readonly law?: ChannelLaw;
  private guards = new Map<number, Creature>();
  private guardPosts = new Map<number, { def: string; post: Vec3; route: Vec3[] | undefined; downMs: number | null }>();
  private zones: TownZone[] = [];
  private watchMs = 0;
  private nav: NavMesh | undefined;
  /** Staff dev spawns: they never pay loot or experience, so they can't be farmed for gold. */
  /** Test monsters from the Developer page: who made each one, and when. */
  private devSpawned = new Map<number, { heroId: number; at: number }>();
  private guardAi?: AiDirector;

  constructor(
    readonly region: RegionDef,
    readonly channelId: string,
    private s: WorldServices,
    /** Shared by every channel of one app: health and Will survive a channel change or reconnect. */
    private carried: Map<number, CarriedVitals> = new Map(),
  ) {
    const hooks: AiHooks = {
      onSpawn: (c) => this.ai?.onSpawn?.(c),
      onDamaged: (id, by, n) => this.ai?.onDamaged?.(id, by, n),
      onDeath: (id, killer) => this.ai?.onDeath?.(id, killer),
    };
    this.nav = s.nav?.(region.name);
    const harm = s.harm ?? defaultHarmPolicy;
    // Creatures don't fight each other, except the town watch and monsters.
    this.combat = new CombatSystem(s.data, hooks, (a, t) => harm(a, t) || this.watchFight(a, t));
    this.combat.onDeath((d) => this.onDeath(d));
    this.combat.onHit((a, t, _dmg, killed) => {
      const now = Date.now();
      if (a.kind === 'hero') this.lastHit.set(a.id, now);
      if (t.kind === 'hero') this.lastHit.set(t.id, now);
      const by = a.kind === 'hero' ? this.heroes.get(a.id) : undefined;
      if (by) by.calmMs = 0;
      const slot = t.kind === 'hero' ? this.heroes.get(t.id) : undefined;
      if (slot) slot.calmMs = 0;
      if (slot && !killed && slot.actAnim !== Anim.Attack) this.showAct(slot, Anim.Hit, FLINCH_SHOW_MS);
    });
    this.spawns = new SpawnManager(s.spawns.regions[region.name], s.data, this.combat, hooks, s.spawnOptions);
    this.ai = s.ai?.({
      region,
      channelId,
      combat: this.combat,
      creatures: () => this.spawns.creatures(),
      heroes: () => [...this.heroes.values()].map((h) => h.hero),
      guards: () => [...this.guards.values()].filter((g) => g.alive),
    });
    this.spawns.start();
    if (s.law) this.law = this.startLaw(s.law);
  }

  // ---------------------------------------------------------------- the town watch

  private startLaw(services: LawServices): ChannelLaw {
    const self = this;
    const port = {
      // The AI's heavy swing is the monster profiles' word; heroes and guards share combat's kinds.
      attack: (a: number, t: number, kind: string) => void this.combat.attack(a, t, kind === 'ranged' ? 'ranged' : 'light'),
      setBlocking: (id: number, on: boolean) => this.combat.setBlocking(id, on),
    };
    let law: ChannelLaw | undefined;
    const world: AiWorld = {
      nav: this.nav,
      creatures: this.guards as unknown as ReadonlyMap<number, AiCreature>,
      combat: port,
      *targets() {
        for (const { hero } of self.heroes.values()) yield { id: hero.id, pos: hero.pos, alive: hero.alive, faction: 'hero' };
        // Monsters too: the watch fights the ones that come into town.
        for (const c of self.spawns.creatures()) if (c.alive && HOSTILE_FACTIONS.has(c.faction)) yield c;
      },
      isHostile: (c, t) => (t.faction === 'hero' ? (law?.guardHostile(c, t) ?? false) : this.inTown(t)),
    };
    this.guardAi = new AiDirector(world, this.channelId.length);
    const out = services.out;
    law = new ChannelLaw(
      {
        channelId: this.channelId,
        region: this.region.name,
        nav: this.nav,
        creatures: () => [...this.guards.values(), ...this.spawns.creatures()] as unknown as AiCreature[],
        *heroes() {
          for (const { hero } of self.heroes.values()) yield { id: hero.id, pos: hero.pos, alive: hero.alive };
        },
        ai: this.guardAi,
        send: (heroId, msg) => out?.send(heroId, msg),
        broadcast: (msg) => out?.broadcast(this.channelId, msg),
      },
      services,
    );
    this.combat.onHit((a, t, dmg, killed) => {
      if (a.kind !== 'hero') return;
      law!.onHarm(
        a.id,
        {
          id: t.id,
          kind: t.kind,
          faction: t.faction,
          maxHealth: t.maxHealth,
          ...(t.kind === 'creature' ? { defName: (t as Creature).defName } : { pvp: (t as Hero).pvp }),
        },
        dmg,
        killed,
      );
    });
    return law;
  }

  devDespawn(heroId: number, what: 'nearest' | 'near'): { removed: string[] } | { error: string } {
    const h = this.heroes.get(heroId)?.hero;
    if (!h) return { error: 'Your hero is not in this region on the server yet.' };
    const reach = what === 'nearest' ? 50 : 30;
    const near = [...this.spawns.creatures()]
      .map((c) => ({ c, d: Math.hypot(c.pos.x - h.pos.x, c.pos.y - h.pos.y, c.pos.z - h.pos.z) }))
      .filter((e) => e.d <= reach)
      .sort((a, b) => a.d - b.d);
    const pick = what === 'nearest' ? near.slice(0, 1) : near;
    if (!pick.length) return { error: `No server monsters within ${reach} m.` };
    const removed: string[] = [];
    for (const { c } of pick) {
      this.devSpawned.delete(c.id);
      if (this.spawns.remove(c.id)) removed.push(c.defName);
    }
    return { removed };
  }

  devSpawn(heroId: number, defName: string, distance: number, side = 0): { id: number } | { error: string } {
    const h = this.heroes.get(heroId)?.hero;
    if (!h) return { error: 'Your hero is not in this region on the server yet.' };
    const d = this.s.data.creatures[defName];
    if (!d) return { error: `The server has no creature ${defName}.` };
    if (!HOSTILE_FACTIONS.has(d.faction ?? '')) return { error: `${defName} is not a monster or bandit.` };
    // Forward is (-sin yaw, cos yaw) and right is (cos yaw, sin yaw), as in the movement step.
    const s = Math.sin(h.yaw), c0 = Math.cos(h.yaw);
    const at = { x: h.pos.x - s * distance + c0 * side, y: h.pos.y + c0 * distance + s * side, z: h.pos.z };
    // Turned round to face the hero.
    const c = this.spawns.spawnExtra(defName, at, h.yaw + Math.PI);
    if (c) {
      this.devSpawned.set(c.id, { heroId, at: Date.now() });
      this.s.events.emit({ type: 'dev_spawn', heroId, creature: defName, id: c.id, channelId: this.channelId });
      // A test monster is there to be fought: it comes for whoever spawned it at once,
      // rather than waiting to spot them (it used to stand about for ten seconds).
      this.ai?.pursue?.(c.id, heroId);
    }
    return c ? { id: c.id } : { error: 'No walkable ground in front of you.' };
  }

  /**
   * The region's towns. Guards fight monsters inside them, and a drawn weapon only gets a
   * warning there. Without zones (old town data) the whole region counts as town.
   */
  setTownZones(zones: TownZone[]): void {
    this.zones = zones;
    this.law?.setZones(zones);
  }

  /** In one of the region's towns (or near enough that the watch steps in). */
  inTown(p: { pos: { x: number; y: number }; alive?: boolean }): boolean {
    if (p.alive === false) return false;
    return !this.zones.length || inAnyZone(p.pos, this.zones, TOWN_WATCH.zoneMargin);
  }

  /** The watch against monsters, both ways. Never guards against townsfolk or each other. */
  private watchFight(a: { kind: string; faction: string }, t: { kind: string; faction: string }): boolean {
    if (a.kind !== 'creature' || t.kind !== 'creature') return false;
    return (a.faction === 'guard' && HOSTILE_FACTIONS.has(t.faction)) || (t.faction === 'guard' && HOSTILE_FACTIONS.has(a.faction));
  }

  /**
   * A monster fighting in town: send the nearest idle guards at it (up to perMonster each),
   * even ones too far away to see it. They are pulled off again by the AI once the monster
   * leaves town (isHostile turns false and they walk back to their beat).
   */
  private stepTownWatch(dtMs: number): void {
    if ((this.watchMs -= dtMs) > 0 || !this.guardAi) return;
    this.watchMs = TOWN_WATCH.thinkMs;
    const busy = new Map<number, number>();
    for (const g of this.guards.values()) if (g.alive && g.targetId !== null) busy.set(g.targetId, (busy.get(g.targetId) ?? 0) + 1);
    for (const m of this.spawns.creatures()) {
      if (!m.alive || !HOSTILE_FACTIONS.has(m.faction) || !this.inTown(m)) continue;
      let need = TOWN_WATCH.perMonster - (busy.get(m.id) ?? 0);
      if (need <= 0) continue;
      const free = [...this.guards.values()]
        .filter((g) => g.alive && g.targetId === null && dist2d(g.pos, m.pos) <= TOWN_WATCH.respondRadius && this.canReach(g.pos, m.pos))
        .sort((a, b) => dist2d(a.pos, m.pos) - dist2d(b.pos, m.pos));
      for (const g of free) {
        if (need <= 0) break;
        if (this.guardAi.pursue(g.id, m.id)) need--;
      }
    }
  }

  /** Ground joins the two spots (always true without a navmesh). A guard who can't get there isn't sent. */
  private canReach(a: { x: number; y: number }, b: { x: number; y: number }): boolean {
    const nav = this.nav;
    if (!nav) return true;
    const ca = nav.nearestCell(a.x, a.y, 2);
    const cb = nav.nearestCell(b.x, b.y, 4);
    return !!ca && !!cb && nav.connected(ca, cb);
  }

  /** Post a guard (the town layer places them at its villages), walking `route` if given. Returns its id. */
  addGuard(defName: string, at: Vec3, route?: Vec3[]): number {
    const id = allocCreatureId();
    if (this.nav && !this.nav.walkable(at.x, at.y)) at = this.nav.snap(at, 8) ?? at;
    let c: Creature;
    try {
      c = creatureFromDef(id, defName, at, this.s.data);
    } catch {
      c = creatureFromDef(id, defName, at, {
        ...this.s.data,
        creatures: { [defName]: { health: GUARD_FALLBACK.health, attackScale: GUARD_FALLBACK.power, experience: 0 } as never },
      });
      c.armour = GUARD_FALLBACK.armour;
    }
    c.faction = 'guard';
    c.experience = 0;
    this.guards.set(id, c);
    if (!this.guardPosts.has(id)) this.guardPosts.set(id, { def: defName, post: { ...at }, route, downMs: null });
    this.combat.add(c);
    this.guardAi?.onSpawn(c as unknown as AiCreature);
    if (route) this.guardAi?.setPatrol(id, route);
    return id;
  }

  private stepGuards(dtMs: number): void {
    for (const [id, g] of this.guardPosts) {
      if (g.downMs === null || (g.downMs -= dtMs) > 0) continue;
      this.guardPosts.delete(id);
      const old = this.guards.get(id);
      if (old) {
        this.guards.delete(id);
        this.combat.remove(id);
        this.guardAi?.remove(id);
      }
      this.addGuard(g.def, g.post, g.route);
    }
    this.stepTownWatch(dtMs);
    this.guardAi?.step(dtMs);
    this.law?.step(dtMs);
  }

  addHero(heroId: number, at: Vec3): void {
    const l = this.s.loadout?.(heroId) ?? {};
    const hero = heroCombatant(heroId, at, this.s.data, l, (i) => {
      if (i.reason === 'staff_only') this.s.events.emit({ type: 'staff_item_blocked', heroId, defId: i.defId, action: 'equip', actor: `hero:${heroId}` });
    });
    const c = this.carried.get(heroId);
    this.carried.delete(heroId);
    if (c && Date.now() - c.at <= CARRY_EXPIRE_MS) {
      hero.health = Math.max(1, Math.round(hero.maxHealth * c.health));
      hero.will = Math.round(hero.maxWill * c.will);
    }
    this.heroes.set(heroId, { hero, move: [0, 0], buttons: 0, home: { ...at }, downMs: null, phial: false, actMs: 0, actAnim: 0, queued: null, queuedMs: 0, calmMs: 0 });
    this.combat.add(hero);
  }

  /** Re-read the hero's loadout and swap in its weapon and armour. Health and the rest stay. */
  refreshLoadout(heroId: number): void {
    const slot = this.heroes.get(heroId);
    if (!slot) return;
    const h = slot.hero;
    const l = this.s.loadout?.(heroId) ?? {};
    const fresh = heroCombatant(heroId, h.pos, this.s.data, l, (i) => {
      if (i.reason === 'staff_only') this.s.events.emit({ type: 'staff_item_blocked', heroId, defId: i.defId, action: 'equip', actor: `hero:${heroId}` });
    });
    h.weapon = fresh.weapon;
    h.armour = fresh.armour;
    h.metal = fresh.metal;
    if (fresh.armourFixed !== undefined) h.armourFixed = fresh.armourFixed;
    else delete h.armourFixed;
  }

  /**
   * The hero's levels changed (bought with experience): take the new stats from the loadout,
   * keeping the same share of health and Will.
   */
  refreshHero(heroId: number): void {
    const slot = this.heroes.get(heroId);
    const l = this.s.loadout?.(heroId);
    if (!slot || !l) return;
    const h = slot.hero;
    const fresh = heroCombatant(heroId, h.pos, this.s.data, l);
    const hp = h.maxHealth > 0 ? h.health / h.maxHealth : 1;
    const will = h.maxWill > 0 ? h.will / h.maxWill : 1;
    h.strength = fresh.strength;
    h.skill = fresh.skill;
    h.willLevel = fresh.willLevel;
    h.spells = fresh.spells;
    h.maxHealth = fresh.maxHealth;
    h.maxWill = fresh.maxWill;
    if (h.alive) h.health = Math.max(1, Math.round(h.maxHealth * hp));
    h.will = Math.round(h.maxWill * will);
  }

  removeHero(heroId: number): Vec3 | undefined {
    const h = this.heroes.get(heroId);
    if (h) {
      const now = Date.now();
      for (const [id, c] of this.carried) if (now - c.at > CARRY_EXPIRE_MS) this.carried.delete(id);
      const hero = h.hero;
      const health = !hero.alive ? RESPAWN_HEALTH_SHARE : hero.maxHealth > 0 ? hero.health / hero.maxHealth : 1;
      const will = hero.maxWill > 0 ? hero.will / hero.maxWill : 1;
      this.carried.set(heroId, { health, will, at: now });
    }
    this.heroes.delete(heroId);
    this.law?.heroLeft(heroId);
    this.charging.delete(heroId);
    this.lastHit.delete(heroId);
    this.combat.remove(heroId);
    return h && { ...h.hero.pos };
  }

  /** Put the hero where the mod says it is (checked by Channel.reportPosition). Not while dead. */
  placeHero(heroId: number, at: Vec3, yaw?: number): boolean {
    const slot = this.heroes.get(heroId);
    if (!slot || !slot.hero.alive) return false;
    slot.hero.pos = { x: at.x, y: at.y, z: at.z };
    if (yaw !== undefined) slot.hero.yaw = yaw;
    slot.move = [0, 0];
    return true;
  }

  applyInput(heroId: number, input: PlayerInput): void {
    const slot = this.heroes.get(heroId);
    if (!slot) return;
    const h = slot.hero;
    const prev = slot.buttons;
    const b = input.buttons;
    slot.buttons = b;
    h.yaw = input.cameraYaw;
    slot.move = [input.moveX, input.moveY];
    if (!h.alive) return;

    this.combat.setBlocking(heroId, (b & BUTTONS.BLOCK) !== 0);
    // Attack fires on release: a short press is a normal swing, a long hold a flourish.
    if (b & BUTTONS.ATTACK) this.charging.add(heroId);
    else if (prev & BUTTONS.ATTACK) {
      this.charging.delete(heroId);
      const kind = h.weapon?.class === 'ranged' ? 'ranged' : h.chargeMs >= BALANCE.flourishChargeMs ? 'flourish' : 'light';
      const r = this.combat.attack(heroId, null, kind);
      if (r.ok) this.showAct(slot, Anim.Attack, SWING_SHOW_MS);
      // Fable queues a press made late in a swing or a flinch; dropping it made most presses
      // do nothing. One press is held at most, and it is shown only when it fires.
      else if ((r.reason === 'cooldown' || r.reason === 'stunned') && Math.max(h.attackCooldownMs, h.stunnedMs) <= BALANCE.swingBufferMs) {
        slot.queued = kind === 'flourish' ? 'light' : kind;
        slot.queuedMs = BALANCE.swingBufferMs;
      }
    }
    if (b & BUTTONS.CAST && !(prev & BUTTONS.CAST)) {
      const spell = SPELL_SLOTS[((b >> SPELL_SHIFT) & 0xf) - 1];
      if (spell) this.combat.cast(heroId, spell, null);
    }
  }

  step(dtMs: number): void {
    const d = (CombatSim.RUN_SPEED * dtMs) / 1000;
    for (const slot of this.heroes.values()) {
      const h = slot.hero;
      if (!h.alive) {
        if (slot.downMs !== null && (slot.downMs -= dtMs) <= 0) this.respawn(slot);
        continue;
      }
      if ((slot.calmMs += dtMs) >= HEALTH_REGEN.calmMs && h.health < h.maxHealth) {
        h.health = Math.min(h.maxHealth, h.health + (h.maxHealth * HEALTH_REGEN.sharePerSec * dtMs) / 1000);
      }
      if (slot.actMs > 0 && (slot.actMs -= dtMs) <= 0) slot.actAnim = 0;
      const [mx, my] = slot.move;
      if ((mx === 0 && my === 0) || h.stunnedMs > 0) {
        h.anim = this.actAnim(slot, 0);
        continue;
      }
      // Blocking and winding up a flourish slow the hero down, as in Fable.
      const k = h.blocking || h.chargeMs > 0 ? 0.4 : 1;
      const s = Math.sin(h.yaw);
      const c = Math.cos(h.yaw);
      h.pos.x += (mx * c - my * s) * d * k;
      h.pos.y += (mx * s + my * c) * d * k;
      h.anim = this.actAnim(slot, 1);
    }
    this.combat.step(dtMs, this.charging);
    for (const [id, slot] of this.heroes) {
      const h = slot.hero;
      if (!slot.queued) continue;
      if (!h.alive || h.blocking || (slot.queuedMs -= dtMs) < 0) slot.queued = null;
      else if (h.attackCooldownMs <= 0 && h.stunnedMs <= 0) {
        this.combat.attack(id, null, slot.queued);
        this.showAct(slot, Anim.Attack, SWING_SHOW_MS);
        slot.queued = null;
      }
    }
    this.ai?.step?.(dtMs);
    this.spawns.setHeroes([...this.heroes.values()].map((h) => h.hero.pos));
    this.spawns.step(dtMs);
    if (this.law) this.stepGuards(dtMs);
  }

  private showAct(slot: HeroSlot, anim: number, ms: number): void {
    slot.actAnim = anim;
    slot.actMs = ms;
  }

  /** What other players see: a swing or flinch in progress, then a raised guard, then `moving`. */
  private actAnim(slot: HeroSlot, moving: number): number {
    if (slot.actMs > 0 && slot.actAnim) return slot.actAnim;
    if (slot.hero.blocking) return Anim.Block;
    return moving;
  }

  snapshot(): EntityState[] {
    const out: EntityState[] = [];
    for (const { hero: h } of this.heroes.values()) out.push({ ...state(h, 0), max_health: Math.ceil(h.maxHealth) });
    // The def name is what the mod spawns as its stand-in; without it a monster shows as a hero.
    for (const c of this.spawns.creatures()) out.push({ ...state(c, fableHash(c.defName)), def: c.defName });
    for (const c of this.guards.values()) out.push({ ...state(c, fableHash(c.defName)), def: c.defName });
    return out;
  }

  position(heroId: number): Vec3 | undefined {
    const p = this.heroes.get(heroId)?.hero.pos;
    return p && { ...p };
  }

  combatState(heroId: number): { alive: boolean; lastHitAt: number } | undefined {
    const h = this.heroes.get(heroId)?.hero;
    return h && { alive: h.alive, lastHitAt: this.lastHit.get(heroId) ?? 0 };
  }

  hero(heroId: number): Hero | undefined {
    return this.heroes.get(heroId)?.hero;
  }

  readonly healing: SimHealing = {
    state: (heroId) => {
      const h = this.heroes.get(heroId)?.hero;
      if (!h) return undefined;
      return { alive: h.alive, health: h.health, maxHealth: h.maxHealth, spellHeal: this.combat.healLifePreview(heroId) };
    },
    castHealLife: (heroId) => this.combat.cast(heroId, 'heal_life', null).ok,
    addHealth: (heroId, amount) => this.combat.addHealth(heroId, amount),
  };

  dispose(): void {
    this.ai?.dispose?.();
    this.spawns.dispose();
    for (const id of this.heroes.keys()) this.combat.remove(id);
    this.heroes.clear();
  }

  // ---------------------------------------------------------------- deaths

  private onDeath(d: DeathInfo): void {
    const v = d.victim;
    if (v.kind === 'creature') {
      const c = v as Creature;
      const post = this.guardPosts.get(c.id);
      if (post) {
        // Killing the watch never pays: no loot, no experience. The post is filled again later.
        this.guardAi?.onDeath(c.id);
        post.downMs = GUARD_RESPAWN_MS;
        return;
      }
      this.spawns.died(c.id);
      const dev = this.devSpawned.get(c.id);
      if (dev) {
        // A test monster pays nothing, but its death is logged so a test fight can be checked.
        this.devSpawned.delete(c.id);
        let heroDamage = 0;
        for (const [id, n] of d.damageBy) if (this.heroes.has(id)) heroDamage += n;
        this.s.events.emit({ type: 'dev_kill', heroId: dev.heroId, creature: c.defName, id: c.id, channelId: this.channelId, killer: d.killer ? `${d.killer.kind}:${d.killer.id}` : null, heroDamage: Math.round(heroDamage), aliveMs: Date.now() - dev.at });
        return;
      }
      // Credit goes to the hero who did the most damage, so kill-stealing a nearly dead monster doesn't pay.
      let best: number | null = null;
      let most = 0;
      let heroDmg = 0;
      let all = 0;
      for (const [id, n] of d.damageBy) {
        all += n;
        if (this.heroes.has(id)) heroDmg += n;
        if (n > most && this.heroes.has(id)) [best, most] = [id, n];
      }
      if (best === null) return;
      if (heroDmg < all * TOWN_WATCH.heroShareForLoot) {
        this.s.events.emit({ type: 'kill_by_watch', heroId: best, creature: c.defName, channelId: this.channelId });
        return;
      }
      const killer = this.heroes.get(best)!.hero;
      const xp = Math.round(c.experience * (1 + Math.min(killer.combo, BALANCE.comboCap) * BALANCE.comboStep));
      this.s.progress?.kill(best, xp);
      const style = d.styleBy.get(best) ?? { strength: 0, skill: 0, will: 0 };
      this.s.experience?.grantKill(best, xp, style, BALANCE.xpGeneralShare, c.defName);
      const loot = this.s.loot?.award(best, c) ?? { gold: 0, items: [] };
      this.s.events.emit({ type: 'kill', heroId: best, creature: c.defName, channelId: this.channelId, experience: xp, gold: loot.gold, items: loot.items.length });
      return;
    }
    const slot = this.heroes.get(v.id);
    if (!slot) return;
    // A flourish being wound up is lost with the fall; the button is read afresh after.
    this.charging.delete(v.id);
    (v as Hero).chargeMs = 0;
    slot.queued = null;
    // Felled by the watch: the law takes over (arrest and a cell), so no phial, penalty or respawn.
    if (this.law?.onHeroDowned(v.id, d.killer?.id ?? null)) {
      this.combat.revive(v.id, 0.5);
      return;
    }
    const pvp = d.killer?.kind === 'hero';
    if (pvp) {
      this.s.progress?.pvpKill(d.killer!.id);
      this.s.events.emit({ type: 'pvp_kill', heroId: d.killer!.id, victimId: v.id, channelId: this.channelId });
    }
    this.s.progress?.death(v.id);
    slot.phial = this.usePhial(v.id);
    slot.downMs = slot.phial ? DEATH.phialMs : DEATH.respawnMs;
    if (!slot.phial && !pvp) this.deathPenalty(v.id);
    const killer = d.killer ? (d.killer.kind === 'hero' ? `hero:${d.killer.id}` : (d.killer as Creature).defName) : null;
    this.s.events.emit({ type: 'hero_death', heroId: v.id, killer, pvp, channelId: this.channelId, phialUsed: slot.phial });
  }

  private usePhial(heroId: number): boolean {
    const eco = this.s.economy;
    if (!eco) return false;
    const phial = eco.inventory(heroId).find((i) => RESURRECTION_PHIALS.includes(i.def_id) && i.location === 'inventory');
    if (!phial) return false;
    try {
      eco.destroyItem(heroId, phial.id, `phial:${phial.id}`);
      return true;
    } catch (e) {
      if (e instanceof GameError) return false;
      throw e;
    }
  }

  private deathPenalty(heroId: number): void {
    const eco = this.s.economy;
    if (!eco) return;
    try {
      const lose = Math.min(DEATH.goldPenaltyMax, Math.floor(eco.balance(heroId) * DEATH.goldPenalty));
      if (lose > 0) eco.spend(heroId, lose, 'death_penalty', `death:${heroId}:${Date.now()}:${Math.random().toString(36).slice(2)}`, 'system');
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
    }
  }

  private respawn(slot: HeroSlot): void {
    const h = slot.hero;
    const moveAway = !slot.phial && !!this.s.respawn;
    // The respawn gate is picked from where the hero fell, so they stay there until it is.
    if (!slot.phial && !moveAway) h.pos = { ...slot.home };
    this.combat.revive(h.id, slot.phial ? 1 : RESPAWN_HEALTH_SHARE);
    h.will = h.maxWill;
    h.combo = 0;
    slot.downMs = null;
    slot.phial = false;
    // Moving between channels is not allowed mid-step: do it once the step is over.
    if (moveAway) {
      const id = h.id;
      const fell = { ...h.pos };
      queueMicrotask(() => {
        if (this.heroes.get(id) !== slot || this.s.respawn!(id, fell)) return;
        // Nowhere to send them: back at the entry point, as without a respawn service.
        h.pos = { ...slot.home };
        this.s.moved?.(id, h.pos);
      });
    }
  }
}

function state(c: Hero | Creature, defId: number): EntityState {
  const r = (n: number) => Math.round(n * 100) / 100;
  return { id: c.id, def_id: defId, x: r(c.pos.x), y: r(c.pos.y), z: r(c.pos.z), yaw: r(c.yaw), anim: c.alive ? c.anim : Anim.Death, health: Math.ceil(c.health) };
}

export const combatSimFactory = (s: WorldServices): SimFactory => {
  // One record per app, not per module: tests run many apps that reuse hero ids.
  const carried = new Map<number, CarriedVitals>();
  return (region, channelId) => new CombatSim(region, channelId, s, carried);
};

const dist2d = (a: { x: number; y: number }, b: { x: number; y: number }) => Math.hypot(a.x - b.x, a.y - b.y);
