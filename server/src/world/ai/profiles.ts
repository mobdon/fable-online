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
};

export const PROFILES: Record<string, AiProfile> = {
  default: base,
  bandit: {
    ...base,
    family: 'bandit',
    runSpeed: 5.5,
    sight: 15,
    fov: 180,
    attackRange: 2.2,
    leash: 45,
    fleeAt: 0.15,
    breaksAfterDeaths: 3,
    packRadius: 14,
    blockChance: 0.2,
    attacks: { light: 3, heavy: 1 },
  },
  banditArcher: {
    ...base,
    family: 'banditArcher',
    runSpeed: 5.5,
    sight: 22,
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
    attacks: { light: 4, heavy: 1 },
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
  },
};

const RULES: [RegExp, string][] = [
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
