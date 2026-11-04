import type { GameData } from '../data/game-data.js';
import type { AiHooks, AttackKind, AttackResult, Combatant, CombatPort, Creature, Hero } from '../world/entities.js';
import {
  BALANCE,
  SPELLS,
  angleDiff,
  angleTo,
  armourReduction,
  baseDamage,
  distance,
  facingAngle,
  inReach,
  isSpell,
  meleeArc,
  weaponClassKey,
  type SpellId,
} from './rules.js';

/** May `a` hurt `t`? Decided by the server: PvP flags, factions, safe zones. */
export type HarmPolicy = (a: Combatant, t: Combatant) => boolean;

export interface DeathInfo {
  victim: Combatant;
  killer: Combatant | null;
  /** Damage each attacker did to the victim this life, for loot and experience credit. */
  damageBy: Map<number, number>;
  /** The same damage split by how it was done (melee, ranged, Will), which decides the experience pools. */
  styleBy: Map<number, DamageStyle>;
}

/** Damage by discipline: melee is Strength, arrows and bolts are Skill, spells are Will. */
export interface DamageStyle {
  strength: number;
  skill: number;
  will: number;
}
export type DeathListener = (d: DeathInfo) => void;
/** Every landed hit, after armour and shields. `killed` is true when it was the killing blow. */
export type HitListener = (attacker: Combatant, target: Combatant, damage: number, killed: boolean) => void;

/** Heroes only hurt heroes when both have PvP on. Creatures don't fight each other. */
export const defaultHarmPolicy: HarmPolicy = (a, t) => {
  if (a.id === t.id) return false;
  if (a.kind === 'hero' && t.kind === 'hero') return (a as Hero).pvp && (t as Hero).pvp;
  if (a.kind === 'creature' && t.kind === 'creature') return false;
  return true;
};

/**
 * Server-side melee, ranged and Will combat for one channel. Clients and AI only ask
 * ("swing", "block", "cast"); this decides reach, facing, cooldowns, blocks, armour and
 * damage, and owns health. Nothing here trusts a number from a client.
 */
export class CombatSystem implements CombatPort {
  readonly all = new Map<number, Combatant>();
  private damageBy = new Map<number, Map<number, number>>();
  private styleBy = new Map<number, Map<number, DamageStyle>>();
  private stunImmuneMs = new Map<number, number>();
  private knockedDown = new Set<number>();
  private spellCooldown = new Map<string, number>();
  private deathListeners: DeathListener[] = [];
  private hitListeners: HitListener[] = [];
  private healLevels: readonly number[];

  constructor(
    data: GameData,
    private hooks: AiHooks = {},
    private canHarm: HarmPolicy = defaultHarmPolicy,
  ) {
    // Heal Life's per-level amounts come straight from game.bin when the extractor found them.
    this.healLevels = data.spells['SPECIAL_ABILITIES_HEAL_LIFE_DEF_INSTANCE']?.levels['2dda34f8'] ?? SPELLS.heal_life.heal;
  }

  onHit(fn: HitListener): void {
    this.hitListeners.push(fn);
  }

  onDeath(fn: DeathListener): void {
    this.deathListeners.push(fn);
  }

  add(c: Combatant): void {
    this.all.set(c.id, c);
    this.damageBy.delete(c.id);
    this.styleBy.delete(c.id);
  }

  remove(id: number): void {
    this.all.delete(id);
    this.damageBy.delete(id);
    this.styleBy.delete(id);
    this.stunImmuneMs.delete(id);
    this.knockedDown.delete(id);
    for (const k of [...this.spellCooldown.keys()]) if (k.startsWith(`${id}:`)) this.spellCooldown.delete(k);
  }

  get(id: number): Combatant | undefined {
    return this.all.get(id);
  }

  setBlocking(id: number, on: boolean): void {
    const c = this.all.get(id);
    if (!c || !c.alive) return;
    c.blocking = on && c.stunnedMs <= 0;
    if (c.blocking && c.kind === 'hero') (c as Hero).chargeMs = 0;
  }

  attack(attackerId: number, targetId: number | null, kind: AttackKind): AttackResult {
    const a = this.all.get(attackerId);
    if (!a || !a.alive) return { ok: false, reason: 'dead' };
    if (a.stunnedMs > 0) return { ok: false, reason: 'stunned' };
    if (a.attackCooldownMs > 0) return { ok: false, reason: 'cooldown' };
    const ranged = a.weapon?.class === 'ranged';
    if (kind === 'ranged' && !ranged) return { ok: false, reason: 'not_allowed' };
    if (ranged) kind = 'ranged';
    if (kind === 'flourish' && a.kind === 'hero') {
      const h = a as Hero;
      if (h.chargeMs < BALANCE.flourishChargeMs) return { ok: false, reason: 'not_allowed' };
    }
    if (a.kind === 'hero') (a as Hero).chargeMs = 0;
    a.blocking = false;
    a.attackCooldownMs = BALANCE.cooldownMs[kind === 'flourish' ? 'flourish' : weaponClassKey(a.weapon)] ?? 600;

    const t = targetId === null ? this.pickTarget(a, ranged) : this.all.get(targetId);
    if (!t || !t.alive || !this.canHarm(a, t) || !inReach(a, t, ranged)) return { ok: true, hit: false, reason: 'miss' };

    let dmg = baseDamage(a as Hero, ranged) * (kind === 'flourish' ? BALANCE.flourishMultiplier : 1);
    let blocked = false;
    if (t.blocking && angleDiff(facingAngle(t), angleTo(t, a)) <= BALANCE.blockCone) {
      if (kind === 'flourish') t.blocking = false; // a flourish smashes through a guard
      else {
        dmg *= BALANCE.blockLeak[ranged ? 'ranged' : weaponClassKey(a.weapon)] ?? 0;
        blocked = true;
      }
    }
    const stun = blocked ? 0 : kind === 'flourish' ? BALANCE.flourishStunMs : BALANCE.flinchMs;
    const done = this.applyDamage(a, t, dmg, { armourFactor: ranged ? BALANCE.rangedArmourFactor : 1, stunMs: stun, style: ranged ? 'skill' : 'strength' });
    return { ok: true, hit: true, targetId: t.id, damage: done, blocked, killed: !t.alive };
  }

  /** Cast a Will spell. Heroes only for now; creature casters come with the AI work. */
  cast(casterId: number, spell: string, targetId: number | null): AttackResult {
    const c = this.all.get(casterId);
    if (!c || !c.alive) return { ok: false, reason: 'dead' };
    if (c.kind !== 'hero') return { ok: false, reason: 'not_allowed' };
    if (!isSpell(spell)) return { ok: false, reason: 'unknown_spell' };
    const h = c as Hero;
    if (h.stunnedMs > 0) return { ok: false, reason: 'stunned' };
    const level = Math.min(4, Math.max(0, Math.floor(h.spells[spell] ?? 0)));
    if (level === 0) return { ok: false, reason: 'not_allowed' };
    const cdKey = `${h.id}:${spell}`;
    if ((this.spellCooldown.get(cdKey) ?? 0) > 0) return { ok: false, reason: 'cooldown' };
    const s = SPELLS[spell as SpellId];
    const cost = s.cost[level - 1]!;
    if (h.will < cost) return { ok: false, reason: 'no_will' };

    const i = level - 1;
    if (spell === 'heal_life') {
      this.spend(h, cost, cdKey, s.cooldownMs);
      const heal = this.healLevels[i] ?? SPELLS.heal_life.heal[i]!;
      h.health = Math.min(h.maxHealth, h.health + heal);
      return { ok: true, hit: true, targetId: h.id, damage: -heal, blocked: false, killed: false };
    }
    if (spell === 'physical_shield') {
      this.spend(h, cost, cdKey, s.cooldownMs);
      h.shield = SPELLS.physical_shield.absorb[i]!;
      h.shieldMs = SPELLS.physical_shield.durationMs;
      return { ok: true, hit: true, targetId: h.id, damage: 0, blocked: false, killed: false };
    }

    const sp = s as { damage: readonly number[]; range: number; cone: number };
    const dmg = sp.damage[i]!;
    const targets =
      spell === 'enflame'
        ? [...this.all.values()].filter((t) => t.alive && this.canHarm(h, t) && distance(h, t) <= sp.range)
        : [targetId === null ? this.pickTarget(h, false, sp.range, sp.cone) : this.all.get(targetId)].filter(
            (t): t is Combatant => !!t && t.alive && this.canHarm(h, t) && distance(h, t) <= sp.range + BALANCE.bodyRadius,
          );
    this.spend(h, cost, cdKey, s.cooldownMs);
    if (targets.length === 0) return { ok: true, hit: false, reason: 'miss' };
    let total = 0;
    for (const t of targets) {
      total += this.applyDamage(h, t, dmg, { armourFactor: BALANCE.spellArmourFactor, stunMs: BALANCE.flinchMs, lightning: spell === 'lightning', style: 'will' });
      if (spell === 'force_push') this.knockBack(h, t, 3);
    }
    if (spell === 'drain_life') h.health = Math.min(h.maxHealth, h.health + total);
    const first = targets[0]!;
    return { ok: true, hit: true, targetId: first.id, damage: total, blocked: false, killed: !first.alive };
  }

  /**
   * What Heal Life would heal this hero for right now, or null when they could not cast it
   * (dead, stunned, not learned, on cooldown or short of Will). Nothing is spent.
   */
  healLifePreview(heroId: number): number | null {
    const c = this.all.get(heroId);
    if (!c || !c.alive || c.kind !== 'hero') return null;
    const h = c as Hero;
    if (h.stunnedMs > 0) return null;
    const level = Math.min(4, Math.max(0, Math.floor(h.spells.heal_life ?? 0)));
    if (level === 0 || (this.spellCooldown.get(`${h.id}:heal_life`) ?? 0) > 0) return null;
    if (h.will < SPELLS.heal_life.cost[level - 1]!) return null;
    return this.healLevels[level - 1] ?? SPELLS.heal_life.heal[level - 1]!;
  }

  /** Add health to a living combatant, capped at max. Returns how much was added. */
  addHealth(id: number, amount: number): number {
    const c = this.all.get(id);
    if (!c || !c.alive || !(amount > 0)) return 0;
    const before = c.health;
    c.health = Math.min(c.maxHealth, c.health + amount);
    return c.health - before;
  }

  /** Advance timers: cooldowns, stuns, shields, Will regeneration, flourish charge. */
  step(dtMs: number, charging?: ReadonlySet<number>): void {
    for (const c of this.all.values()) {
      if (c.attackCooldownMs > 0) c.attackCooldownMs = Math.max(0, c.attackCooldownMs - dtMs);
      if (c.stunnedMs > 0) {
        c.stunnedMs = Math.max(0, c.stunnedMs - dtMs);
        if (c.stunnedMs === 0 && this.knockedDown.delete(c.id)) this.stunImmuneMs.set(c.id, BALANCE.stunImmunityMs);
      }
      const im = this.stunImmuneMs.get(c.id);
      if (im !== undefined) im - dtMs <= 0 ? this.stunImmuneMs.delete(c.id) : this.stunImmuneMs.set(c.id, im - dtMs);
      if (c.kind !== 'hero' || !c.alive) continue;
      const h = c as Hero;
      h.will = Math.min(h.maxWill, h.will + (BALANCE.hero.willRegenPerSec * dtMs) / 1000);
      if (h.shieldMs > 0) {
        h.shieldMs = Math.max(0, h.shieldMs - dtMs);
        if (h.shieldMs === 0) h.shield = 0;
      }
      h.chargeMs = charging?.has(h.id) && h.stunnedMs <= 0 && !h.blocking ? h.chargeMs + dtMs : 0;
    }
    for (const [k, v] of this.spellCooldown) v - dtMs <= 0 ? this.spellCooldown.delete(k) : this.spellCooldown.set(k, v - dtMs);
  }

  /** Bring a dead combatant back (respawn, resurrection). */
  revive(id: number, healthShare = 1): void {
    const c = this.all.get(id);
    if (!c) return;
    c.alive = true;
    c.health = Math.max(1, Math.round(c.maxHealth * healthShare));
    c.stunnedMs = 0;
    c.attackCooldownMs = 0;
    c.blocking = false;
    this.damageBy.delete(id);
    this.styleBy.delete(id);
  }

  // ---------------------------------------------------------------- internals

  /** Fable-style soft lock: the closest valid target inside the attack cone. */
  private pickTarget(a: Combatant, ranged: boolean, range?: number, cone?: number): Combatant | undefined {
    let best: Combatant | undefined;
    let bestD = Infinity;
    const arc = ranged ? { reach: (BALANCE.reach.ranged ?? 40) + BALANCE.bodyRadius, cone: BALANCE.rangedCone } : meleeArc(a);
    const r = range !== undefined ? range + BALANCE.bodyRadius : arc.reach;
    const half = cone ?? arc.cone;
    for (const t of this.all.values()) {
      if (t === a || !t.alive || !this.canHarm(a, t)) continue;
      const d = distance(a, t);
      if (d > r || d >= bestD) continue;
      if (angleDiff(facingAngle(a), angleTo(a, t)) > half) continue;
      best = t;
      bestD = d;
    }
    return best;
  }

  private spend(h: Hero, cost: number, cdKey: string, cooldownMs: number): void {
    h.will -= cost;
    this.spellCooldown.set(cdKey, cooldownMs);
    h.chargeMs = 0;
  }

  /** Apply one hit after blocks. Returns the damage actually dealt. */
  private applyDamage(
    a: Combatant,
    t: Combatant,
    raw: number,
    o: { armourFactor: number; stunMs: number; lightning?: boolean; style?: keyof DamageStyle },
  ): number {
    let dmg = raw;
    if (a.kind === 'hero' && t.kind === 'hero') dmg *= BALANCE.pvpScale;
    dmg *= 1 - armourReduction(t.armour, t.armourFixed) * o.armourFactor;
    if (o.lightning && t.metal) dmg *= 1 + BALANCE.lightningMetalBonus;
    dmg = dmg > 0 ? Math.max(1, Math.round(dmg)) : 0;
    if (t.kind === 'hero' && dmg > 0) {
      const h = t as Hero;
      const soak = Math.min(h.shield, dmg);
      h.shield -= soak;
      dmg -= soak;
    }
    if (dmg <= 0) return 0;
    t.health = Math.max(0, t.health - dmg);
    const by = this.damageBy.get(t.id) ?? new Map<number, number>();
    by.set(a.id, (by.get(a.id) ?? 0) + dmg);
    this.damageBy.set(t.id, by);
    if (o.style && a.kind === 'hero') {
      const styles = this.styleBy.get(t.id) ?? new Map<number, DamageStyle>();
      const st = styles.get(a.id) ?? { strength: 0, skill: 0, will: 0 };
      st[o.style] += dmg;
      styles.set(a.id, st);
      this.styleBy.set(t.id, styles);
    }
    if (a.kind === 'hero') (a as Hero).combo = Math.min(BALANCE.comboCap, (a as Hero).combo + 1);
    if (t.kind === 'hero') {
      (t as Hero).combo = 0;
      (t as Hero).chargeMs = 0; // getting hit interrupts a flourish wind-up
    }
    if (o.stunMs > 0 && !this.stunImmuneMs.has(t.id)) {
      t.stunnedMs = Math.max(t.stunnedMs, o.stunMs);
      t.blocking = false;
      if (o.stunMs > BALANCE.flinchMs) this.knockedDown.add(t.id);
    }
    if (t.kind === 'creature') this.hooks.onDamaged?.(t.id, a.id, dmg);
    const killed = t.health <= 0;
    // Listeners hear the hit before the death, so a crime is on the record before any death handling.
    for (const fn of this.hitListeners) fn(a, t, dmg, killed);
    if (killed) this.kill(t, a);
    return dmg;
  }

  private kill(t: Combatant, killer: Combatant | null): void {
    t.alive = false;
    t.blocking = false;
    t.health = 0;
    if (t.kind === 'creature') (t as Creature).targetId = null;
    const info: DeathInfo = { victim: t, killer, damageBy: this.damageBy.get(t.id) ?? new Map(), styleBy: this.styleBy.get(t.id) ?? new Map() };
    this.damageBy.delete(t.id);
    this.styleBy.delete(t.id);
    this.hooks.onDeath?.(t.id, killer?.id ?? null);
    for (const fn of this.deathListeners) fn(info);
  }

  private knockBack(from: Combatant, t: Combatant, metres: number): void {
    const ang = angleTo(from, t);
    t.pos.x += Math.cos(ang) * metres;
    t.pos.y += Math.sin(ang) * metres;
  }
}
