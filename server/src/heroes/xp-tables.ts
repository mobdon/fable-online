/**
 * Fable's experience model, as the server runs it (tools/re-notes/13-experience.md).
 *
 * Fable has no character level. A hero earns general experience and Strength, Skill and
 * Will experience, and spends it on ten "trainable stats" (Physique, Health, Toughness,
 * Speed, Accuracy, Guile, Will; three slots are unused) and on abilities and spells.
 * A purchase is paid from its own discipline's pool first and the rest from general
 * experience, the same rule as Fable's own spend function.
 *
 * The real price tables come from the player's game.bin (src/data/extract.ts writes them
 * into game-data.json). FALLBACK_PROGRESSION below is hand-made, in Fable's ballpark, for
 * tests and for a server set up without the extract step.
 */

export type Discipline = 'strength' | 'skill' | 'will';
export const DISCIPLINES: readonly Discipline[] = ['strength', 'skill', 'will'];

/** Fable's TrainableStatLevels has ten slots; the hero's experience screen uses seven. */
export const STAT_SLOTS = 10;
/** CTCInventoryAbilities holds twenty ability slots (HERO_ABILITY slot field 0..19). */
export const ABILITY_SLOTS = 20;
export const MAX_ABILITY_LEVEL = 4;

/** One trainable stat. `costs[n]` is the price of level n (costs[0] is 0, the starting level). */
export interface StatTrack {
  slot: number;
  id: StatId;
  discipline: Discipline;
  costs: number[];
}

/** One HERO_ABILITY def. `costs[n - 1]` is the price of level n (1..4). */
export interface AbilityTrack {
  slot: number;
  def: string;
  discipline: Discipline;
  costs: number[];
}

export interface Progression {
  stats: StatTrack[];
  abilities: AbilityTrack[];
}

export type StatId = 'physique' | 'health' | 'toughness' | 'speed' | 'accuracy' | 'guile' | 'will';

/**
 * Slot order of TrainableStatLevels, matching the track order in MAIN_HERO_STATS.
 * Slots 6-8 are tracks with no screen text in the retail game; they can't be bought.
 */
export const STAT_BY_SLOT: readonly (StatId | null)[] = ['physique', 'health', 'toughness', 'speed', 'accuracy', 'guile', null, null, null, 'will'];

/** HERO_ABILITY defs the server's combat knows as spells (src/combat/rules.ts SPELLS). */
export const SPELL_OF_ABILITY: Record<string, string> = {
  HERO_ABILITY_WILL_FIREBALL: 'fireball',
  HERO_ABILITY_WILL_LIGHTNING: 'lightning',
  HERO_ABILITY_WILL_DRAIN_LIFE: 'drain_life',
  HERO_ABILITY_WILL_FORCE_PUSH: 'force_push',
  HERO_ABILITY_WILL_ENFLAME: 'enflame',
  HERO_ABILITY_WILL_HEAL_LIFE: 'heal_life',
  HERO_ABILITY_WILL_PHYSICAL_SHIELD_SPELL: 'physical_shield',
};

/** The discipline a HERO_ABILITY def belongs to, from its name (HERO_ABILITY_STENGTH_* is Fable's spelling). */
export function abilityDiscipline(def: string): Discipline {
  if (/^HERO_ABILITY_(STRENGTH|STENGTH)_/.test(def)) return 'strength';
  if (/^HERO_ABILITY_SKILL_/.test(def)) return 'skill';
  return 'will';
}

/** Hand-made tables in Fable's ballpark. Not taken from the game files. */
export const FALLBACK_PROGRESSION: Progression = {
  stats: STAT_BY_SLOT.flatMap((id, slot) =>
    id
      ? [
          {
            slot,
            id,
            discipline: (slot < 3 ? 'strength' : slot < 6 ? 'skill' : 'will') as Discipline,
            costs: [0, 400, 1000, 2500, 7000, 18000, 40000, 80000],
          },
        ]
      : [],
  ),
  abilities: [
    { slot: 1, def: 'HERO_ABILITY_WILL_LIGHTNING', discipline: 'will', costs: [0, 5000, 18000, 45000] },
    { slot: 2, def: 'HERO_ABILITY_WILL_FIREBALL', discipline: 'will', costs: [500, 5000, 18000, 50000] },
    { slot: 3, def: 'HERO_ABILITY_STENGTH_DOUBLE_STRIKE', discipline: 'strength', costs: [1000, 5000, 15000, 45000] },
    { slot: 8, def: 'HERO_ABILITY_WILL_FORCE_PUSH', discipline: 'will', costs: [600, 4000, 12000, 40000] },
    { slot: 9, def: 'HERO_ABILITY_WILL_ENFLAME', discipline: 'will', costs: [3000, 8000, 22000, 55000] },
    { slot: 11, def: 'HERO_ABILITY_WILL_PHYSICAL_SHIELD_SPELL', discipline: 'will', costs: [1000, 5000, 18000, 45000] },
    { slot: 14, def: 'HERO_ABILITY_WILL_DRAIN_LIFE', discipline: 'will', costs: [3000, 7000, 16000, 45000] },
    { slot: 15, def: 'HERO_ABILITY_WILL_HEAL_LIFE', discipline: 'will', costs: [2000, 6000, 16000, 50000] },
  ],
};

/** The tables as one lookup, checked once at start-up. */
export class XpTables {
  readonly statBySlot = new Map<number, StatTrack>();
  readonly abilityBySlot = new Map<number, AbilityTrack>();

  constructor(p: Progression) {
    for (const s of p.stats) if (s.slot >= 0 && s.slot < STAT_SLOTS && s.costs.length >= 2) this.statBySlot.set(s.slot, s);
    // Several defs can name one slot; Fable keeps the last one it reads, and so do we.
    for (const a of p.abilities) if (a.slot >= 0 && a.slot < ABILITY_SLOTS && a.costs.length) this.abilityBySlot.set(a.slot, a);
  }

  statMax(slot: number): number {
    const s = this.statBySlot.get(slot);
    return s ? s.costs.length - 1 : 0;
  }

  abilityMax(slot: number): number {
    const a = this.abilityBySlot.get(slot);
    return a ? Math.min(MAX_ABILITY_LEVEL, a.costs.length) : 0;
  }

  /** XP to go from level `from` to level `to` of a stat, or null if a level doesn't exist. */
  statCost(slot: number, from: number, to: number): number | null {
    const s = this.statBySlot.get(slot);
    if (!s || from < 0 || to > s.costs.length - 1 || to < from) return null;
    let sum = 0;
    for (let n = from + 1; n <= to; n++) sum += s.costs[n]!;
    return sum;
  }

  /** XP to go from level `from` (0 = not owned) to `to` of an ability, or null. */
  abilityCost(slot: number, from: number, to: number): number | null {
    const a = this.abilityBySlot.get(slot);
    if (!a || from < 0 || to > this.abilityMax(slot) || to < from) return null;
    let sum = 0;
    for (let n = from + 1; n <= to; n++) sum += a.costs[n - 1]!;
    return sum;
  }
}

export interface Pools {
  general: number;
  strength: number;
  skill: number;
  will: number;
}

/**
 * Pays `cost` the way Fable's CTCHeroExperience does: from the discipline's own pool,
 * and whatever that pool can't cover from general experience. Returns the pools after
 * paying, or null when the hero can't afford it.
 */
export function pay(p: Pools, d: Discipline, cost: number): Pools | null {
  if (cost <= 0) return { ...p };
  const own = Math.min(p[d], cost);
  const rest = cost - own;
  if (rest > p.general) return null;
  return { ...p, [d]: p[d] - own, general: p.general - rest };
}
