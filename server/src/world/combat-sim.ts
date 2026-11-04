import type { EventSink } from '../analytics/events.js';
import { CombatSystem, defaultHarmPolicy, type DeathInfo, type HarmPolicy } from '../combat/combat.js';
import { heroCombatant, type HeroLoadout } from '../combat/loadout.js';
import { BALANCE, type SpellId } from '../combat/rules.js';
import { fableHash } from '../data/fable-bin.js';
import type { GameData } from '../data/game-data.js';
import type { Economy } from '../economy/economy.js';
import type { LootService } from '../economy/loot.js';
import { GameError } from '../errors.js';
import type { Progress } from '../heroes/progress.js';
import type { AiHooks, Creature, Hero } from './entities.js';
import type { RegionDef } from './regions.js';
import type { EntityState, PlayerInput, RegionSim, SimFactory, SimHealing, Vec3 } from './sim.js';
import { HOSTILE_FACTIONS, SpawnManager, allocCreatureId, type SpawnOptions, type SpawnsFile } from './spawns.js';
import { creatureFromDef } from '../combat/loadout.js';
import { AiDirector } from './ai/brain.js';
import { Anim, type AiWorld, type Creature as AiCreature } from './ai/types.js';
import { ChannelLaw, type LawServices } from '../law/guards.js';

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
  dispose?(): void;
}
export interface AiContext {
  region: RegionDef;
  channelId: string;
  combat: CombatSystem;
  creatures: () => Iterable<Creature>;
  heroes: () => Iterable<Hero>;
}

export interface WorldServices {
  data: GameData;
  spawns: SpawnsFile;
  events: EventSink;
  economy?: Economy;
  loot?: LootService;
  progress?: Progress;
  /** What the hero has equipped and learned, from the server's own records. */
  loadout?: (heroId: number) => HeroLoadout;
  harm?: HarmPolicy;
  ai?: (ctx: AiContext) => ChannelAi;
  spawnOptions?: Partial<SpawnOptions>;
  /** Crime and the town watch. Without it there are no guards and nothing is a crime. */
  law?: LawServices;
}

/** Guard stats when game.bin has no entry for the def (test data, or an unread def). */
const GUARD_FALLBACK = { health: 300, power: 1.2, armour: 20 };
/** A dead guard is replaced at its post after this long. */
export const GUARD_RESPAWN_MS = 60_000;

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
}

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
  private guardPosts = new Map<number, { def: string; post: Vec3; downMs: number | null }>();
  /** Staff dev spawns: they never pay loot or experience, so they can't be farmed for gold. */
  private devSpawned = new Set<number>();
  private guardAi?: AiDirector;

  constructor(
    readonly region: RegionDef,
    readonly channelId: string,
    private s: WorldServices,
  ) {
    const hooks: AiHooks = {
      onSpawn: (c) => this.ai?.onSpawn?.(c),
      onDamaged: (id, by, n) => this.ai?.onDamaged?.(id, by, n),
      onDeath: (id, killer) => this.ai?.onDeath?.(id, killer),
    };
    this.combat = new CombatSystem(s.data, hooks, s.harm ?? defaultHarmPolicy);
    this.combat.onDeath((d) => this.onDeath(d));
    this.combat.onHit((a, t, _dmg, killed) => {
      const now = Date.now();
      if (a.kind === 'hero') this.lastHit.set(a.id, now);
      if (t.kind === 'hero') this.lastHit.set(t.id, now);
      const slot = t.kind === 'hero' ? this.heroes.get(t.id) : undefined;
      if (slot && !killed && slot.actAnim !== Anim.Attack) this.showAct(slot, Anim.Hit, FLINCH_SHOW_MS);
    });
    this.spawns = new SpawnManager(s.spawns.regions[region.name], s.data, this.combat, hooks, s.spawnOptions);
    this.ai = s.ai?.({
      region,
      channelId,
      combat: this.combat,
      creatures: () => this.spawns.creatures(),
      heroes: () => [...this.heroes.values()].map((h) => h.hero),
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
      nav: undefined,
      creatures: this.guards as unknown as ReadonlyMap<number, AiCreature>,
      combat: port,
      *targets() {
        for (const { hero } of self.heroes.values()) yield { id: hero.id, pos: hero.pos, alive: hero.alive, faction: 'hero' };
      },
      isHostile: (c, t) => law?.guardHostile(c, t) ?? false,
    };
    this.guardAi = new AiDirector(world, this.channelId.length);
    const out = services.out;
    law = new ChannelLaw(
      {
        channelId: this.channelId,
        region: this.region.name,
        nav: undefined,
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
    if (c) this.devSpawned.add(c.id);
    return c ? { id: c.id } : { error: 'No walkable ground in front of you.' };
  }

  /** Post a guard (the town layer places them at its villages). Returns its id. */
  addGuard(defName: string, at: Vec3): number {
    const id = allocCreatureId();
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
    if (!this.guardPosts.has(id)) this.guardPosts.set(id, { def: defName, post: { ...at }, downMs: null });
    this.combat.add(c);
    this.guardAi?.onSpawn(c as unknown as AiCreature);
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
      this.addGuard(g.def, g.post);
    }
    this.guardAi?.step(dtMs);
    this.law?.step(dtMs);
  }

  addHero(heroId: number, at: Vec3): void {
    const l = this.s.loadout?.(heroId) ?? {};
    const hero = heroCombatant(heroId, at, this.s.data, l, (i) => {
      if (i.reason === 'staff_only') this.s.events.emit({ type: 'staff_item_blocked', heroId, defId: i.defId, action: 'equip', actor: `hero:${heroId}` });
    });
    this.heroes.set(heroId, { hero, move: [0, 0], buttons: 0, home: { ...at }, downMs: null, phial: false, actMs: 0, actAnim: 0 });
    this.combat.add(hero);
  }

  removeHero(heroId: number): Vec3 | undefined {
    const h = this.heroes.get(heroId);
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
      this.combat.attack(heroId, null, kind);
      this.showAct(slot, Anim.Attack, SWING_SHOW_MS);
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
    for (const c of this.spawns.creatures()) out.push(state(c, fableHash(c.defName)));
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
      if (this.devSpawned.delete(c.id)) return;
      // Credit goes to the hero who did the most damage, so kill-stealing a nearly dead monster doesn't pay.
      let best: number | null = null;
      let most = 0;
      for (const [id, n] of d.damageBy) if (n > most && this.heroes.has(id)) [best, most] = [id, n];
      if (best === null) return;
      const killer = this.heroes.get(best)!.hero;
      const xp = Math.round(c.experience * (1 + Math.min(killer.combo, BALANCE.comboCap) * BALANCE.comboStep));
      this.s.progress?.kill(best, xp);
      const loot = this.s.loot?.award(best, c) ?? { gold: 0, items: [] };
      this.s.events.emit({ type: 'kill', heroId: best, creature: c.defName, channelId: this.channelId, experience: xp, gold: loot.gold, items: loot.items.length });
      return;
    }
    const slot = this.heroes.get(v.id);
    if (!slot) return;
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
    if (!slot.phial) h.pos = { ...slot.home };
    this.combat.revive(h.id, slot.phial ? 1 : 0.5);
    h.will = h.maxWill;
    h.combo = 0;
    slot.downMs = null;
    slot.phial = false;
  }
}

function state(c: Hero | Creature, defId: number): EntityState {
  const r = (n: number) => Math.round(n * 100) / 100;
  return { id: c.id, def_id: defId, x: r(c.pos.x), y: r(c.pos.y), z: r(c.pos.z), yaw: r(c.yaw), anim: c.alive ? c.anim : Anim.Death, health: Math.ceil(c.health) };
}

export const combatSimFactory =
  (s: WorldServices): SimFactory =>
  (region, channelId) =>
    new CombatSim(region, channelId, s);
