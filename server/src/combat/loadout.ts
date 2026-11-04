import type { GameData, WeaponData } from '../data/game-data.js';
import type { Creature, Hero } from '../world/entities.js';
import type { Vec3 } from '../world/sim.js';
import { BALANCE, armourPoints, wearsMetal, type SpellId } from './rules.js';

/** What a hero has on and has learned. Built by the server from the hero's own records. */
export interface HeroLoadout {
  /** Item def ids of equipped gear: one weapon and any clothing pieces. */
  equipped?: string[];
  strength?: number;
  skill?: number;
  willLevel?: number;
  /** Fable's Health stat (0..7). Without it, max health follows Strength as before. */
  health?: number;
  spells?: Partial<Record<SpellId, number>>;
  pvp?: boolean;
  faction?: string;
  /** Staff accounts may wear staff-only gear (platinum). */
  staff?: boolean;
}

export interface LoadoutIssue {
  heroId: number;
  defId: string;
  reason: 'staff_only' | 'unknown_item';
}

const lvl = (n: number | undefined) => Math.max(0, Math.min(7, Math.floor(n ?? 0)));

export function heroCombatant(id: number, pos: Vec3, data: GameData, l: HeroLoadout = {}, onIssue?: (i: LoadoutIssue) => void): Hero {
  let weapon: WeaponData | null = null;
  const pieces: { coverage?: number; armour?: string }[] = [];
  let staff = false;
  for (const defId of l.equipped ?? []) {
    const item = data.items[defId];
    if (!item) {
      onIssue?.({ heroId: id, defId, reason: 'unknown_item' });
      continue;
    }
    if (item.armour === BALANCE.staffArmour) {
      if (!l.staff) {
        onIssue?.({ heroId: id, defId, reason: 'staff_only' });
        continue;
      }
      staff = true;
    }
    if (item.weapon && !weapon) weapon = item.weapon;
    else if (item.coverage) pieces.push(item);
  }
  const strength = lvl(l.strength);
  const willLevel = lvl(l.willLevel);
  const maxHealth = heroMaxHealth(l);
  const maxWill = heroMaxWill(l);
  return {
    id,
    kind: 'hero',
    pos: { ...pos },
    yaw: 0,
    health: maxHealth,
    maxHealth,
    alive: true,
    faction: l.faction ?? 'FACTION_HERO',
    anim: 0,
    blocking: false,
    stunnedMs: 0,
    attackCooldownMs: 0,
    weapon,
    power: 1,
    armour: armourPoints(pieces),
    ...(staff ? { armourFixed: BALANCE.staffReduction } : {}),
    metal: wearsMetal(pieces),
    will: maxWill,
    maxWill,
    strength,
    skill: lvl(l.skill),
    willLevel,
    spells: { ...(l.spells ?? {}) },
    pvp: !!l.pvp,
    combo: 0,
    shield: 0,
    shieldMs: 0,
    chargeMs: 0,
  };
}

/** Max health from the loadout: the Health stat when the server tracks it, else Strength. */
export function heroMaxHealth(l: HeroLoadout): number {
  return BALANCE.hero.baseHealth + lvl(l.health ?? l.strength) * BALANCE.hero.healthPerStrength;
}

export function heroMaxWill(l: HeroLoadout): number {
  return BALANCE.hero.baseWill + lvl(l.willLevel) * BALANCE.hero.willPerLevel;
}

export function creatureFromDef(id: number, defName: string, pos: Vec3, data: GameData, spawnId: string | null = null): Creature {
  const d = data.creatures[defName];
  if (!d) throw new Error(`Unknown creature ${defName}`);
  const weapon = (d.weapon && data.items[d.weapon]?.weapon) || null;
  return {
    id,
    kind: 'creature',
    defName,
    pos: { ...pos },
    yaw: 0,
    health: d.health,
    maxHealth: d.health,
    alive: true,
    faction: d.faction ?? 'FACTION_MONSTERS',
    anim: 0,
    blocking: false,
    stunnedMs: 0,
    attackCooldownMs: 0,
    weapon,
    power: d.attackScale > 0 ? d.attackScale : 1,
    armour: 0,
    targetId: null,
    spawnId,
    experience: d.experience,
  };
}
