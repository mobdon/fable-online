import type { AttackKind, Creature } from './types.js';

/**
 * How a family of creatures behaves. Numbers are first guesses tuned by feel, not taken
 * from Fable; per-def stats from game.bin (Creature.stats) override speeds and ranges.
 */
export interface AiProfile {
  family: string;
  walkSpeed: number; // m/s
  runSpeed: number; // m/s
  /** Sees heroes this far away inside its field of view. */
  sight: number;
  /** Field of view in degrees, centred on facing. */
  fov: number;
  /** Notices heroes this close whatever way it faces. */
  hear: number;
  /** Swing distance (centre to centre). */
  attackRange: number;
  /** Ranged attackers try to stand this far from their target. 0 = melee. */
  preferredRange: number;
  /** Gives up and goes home past this distance from its spawn point. */
  leash: number;
  wanderRadius: number;
  /** Idle pause between wander legs, ms [min, max]. */
  idleMs: [number, number];
  /** Pause after noticing a target before charging (the "huh?" moment players can react to). */
  alertMs: number;
  /** Runs away below this share of max health (0 = never). Only once per fight. */
  fleeAt: number;
  fleeMs: number;
  /** Runs away when this many nearby allies have died this fight (0 = never). */
  breaksAfterDeaths: number;
  /** Alerts idle same-family creatures within this radius when it notices a target. */
  packRadius: number;
  /** Circles the target between attacks instead of standing still (wolves, balverines). */
  circles: boolean;
  /** Lunges from mid range (balverines). */
  leap: { min: number; max: number; speed: number; cooldownMs: number } | null;
  /** Chance per think to raise a guard while waiting on its swing. */
  blockChance: number;
  /** Attack mix: kind -> weight. */
  attacks: Partial<Record<AttackKind, number>>;
  /**
   * Wind-up before a swing lands, ms, by kind. The strike animation starts at the wind-up,
   * so a hero who sees it coming can block, step back or hit first (a flinch cancels it).
   */
  windupMs: Partial<Record<AttackKind, number>>;
  /** Pause after a swing (or the last swing of a combo) before the next one, ms [min, max]. */
  recoverMs: [number, number];
  /** Most swings in one flurry; each extra swing follows after a short gap. */
  comboMax: number;
  /** Chance per think, while recovering, to raise its guard when the target is winding up a swing. */
  reactBlockChance: number;
}

const base: AiProfile = {
  family: 'default',
  walkSpeed: 1.6,
  runSpeed: 5,
  sight: 14,
  fov: 200,
  hear: 4,
  attackRange: 2,
  preferredRange: 0,
  leash: 40,
  wanderRadius: 6,
  idleMs: [2000, 6000],
  alertMs: 600,
  fleeAt: 0,
  fleeMs: 4000,
  breaksAfterDeaths: 0,
  packRadius: 10,
  circles: false,
  leap: null,
  blockChance: 0,
  attacks: { light: 1 },
  windupMs: { light: 450, heavy: 900, ranged: 600 },
  recoverMs: [1200, 2000],
  comboMax: 1,
  reactBlockChance: 0,
};

export const PROFILES: Record<string, AiProfile> = {
  default: base,
  bandit: {
    ...base,
    family: 'bandit',
    runSpeed: 5.5,
    sight: 22,
    fov: 220,
    hear: 8,
    attackRange: 2.2,
    leash: 45,
    alertMs: 350,
    fleeAt: 0.15,
    breaksAfterDeaths: 3,
    packRadius: 14,
    // Fable bandits square up: a flurry of one or two swings, then step round the hero with
    // their guard up before going in again.
    circles: true,
    blockChance: 0.25,
    reactBlockChance: 0.45,
    attacks: { light: 5, heavy: 1 },
    windupMs: { light: 450, heavy: 1000 },
    recoverMs: [1300, 2300],
    comboMax: 2,
  },
  banditArcher: {
    ...base,
    family: 'banditArcher',
    runSpeed: 5.5,
    sight: 26,
    hear: 8,
    fov: 180,
    attackRange: 20,
    preferredRange: 12,
    leash: 45,
    fleeAt: 0.2,
    breaksAfterDeaths: 2,
    packRadius: 14,
    attacks: { ranged: 1 },
  },
  hobbe: {
    ...base,
    family: 'hobbe',
    walkSpeed: 1.4,
    runSpeed: 4.8,
    sight: 12,
    attackRange: 1.8,
    leash: 35,
    alertMs: 400,
    fleeAt: 0.25,
    breaksAfterDeaths: 2,
    packRadius: 12,
    blockChance: 0.1,
    reactBlockChance: 0.2,
    attacks: { light: 4, heavy: 1 },
    windupMs: { light: 400, heavy: 900 },
    recoverMs: [1000, 1800],
    comboMax: 2,
  },
  wolf: {
    ...base,
    family: 'wolf',
    walkSpeed: 2,
    runSpeed: 7.5,
    sight: 18,
    fov: 240,
    hear: 6,
    attackRange: 1.8,
    leash: 50,
    wanderRadius: 10,
    alertMs: 300,
    packRadius: 18,
    circles: true,
    attacks: { light: 1 },
    windupMs: { light: 350 },
    recoverMs: [900, 1600],
  },
  balverine: {
    ...base,
    family: 'balverine',
    walkSpeed: 2,
    runSpeed: 8,
    sight: 20,
    fov: 240,
    hear: 6,
    attackRange: 2.2,
    leash: 55,
    wanderRadius: 10,
    alertMs: 500,
    packRadius: 20,
    circles: true,
    leap: { min: 4, max: 9, speed: 15, cooldownMs: 5000 },
    attacks: { light: 2, heavy: 1 },
    windupMs: { light: 400, heavy: 800 },
    recoverMs: [900, 1600],
    comboMax: 2,
  },
  /**
   * Town guards (CREATURE_BS_GUARD*, CREATURE_KHG_GUARD, CREATURE_BS_SHERIFF, prison guards).
   * They stand near their post, never flee, call every guard in earshot, block a lot, and
   * follow a criminal further than a monster would. Who counts as a criminal comes from the
   * law module through AiWorld.isHostile.
   */
  guard: {
    ...base,
    family: 'guard',
    walkSpeed: 1.4,
    runSpeed: 5.8,
    sight: 18,
    fov: 200,
    hear: 5,
    attackRange: 2.2,
    leash: 120,
    wanderRadius: 3,
    idleMs: [4000, 10000],
    alertMs: 300,
    packRadius: 25,
    blockChance: 0.35,
    reactBlockChance: 0.5,
    attacks: { light: 3, heavy: 1 },
    comboMax: 2,
  },
  guardCrossbow: {
    ...base,
    family: 'guard',
    walkSpeed: 1.4,
    runSpeed: 5.5,
    sight: 24,
    fov: 200,
    hear: 5,
    attackRange: 18,
    preferredRange: 10,
    leash: 120,
    wanderRadius: 3,
    idleMs: [4000, 10000],
    alertMs: 300,
    packRadius: 25,
    attacks: { ranged: 1 },
  },
};

/** Def names of the law's creatures: guards, sheriffs, prison guards. */
export const GUARD_DEF = /GUARD|SHERIFF/;

const RULES: [RegExp, string][] = [
  [/GUARD.*CROSSBOW/, 'guardCrossbow'],
  [GUARD_DEF, 'guard'],
  [/BANDIT.*(ARCHER|BOW)/, 'banditArcher'],
  [/BANDIT/, 'bandit'],
  [/HOBBE/, 'hobbe'],
  [/WOLF/, 'wolf'],
  [/BALVERINE/, 'balverine'],
];

export function profileFor(c: Pick<Creature, 'defName' | 'stats'>): AiProfile {
  const name = c.defName.toUpperCase();
  const family = RULES.find(([re]) => re.test(name))?.[1] ?? 'default';
  const p = PROFILES[family]!;
  const s = c.stats;
  if (!s) return p;
  return {
    ...p,
    walkSpeed: s.walkSpeed ?? p.walkSpeed,
    runSpeed: s.runSpeed ?? p.runSpeed,
    sight: s.sightRange ?? p.sight,
    attackRange: s.attackRange ?? p.attackRange,
    fleeAt: s.flees === false ? 0 : p.fleeAt,
  };
}
