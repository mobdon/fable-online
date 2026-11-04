import fs from 'node:fs';

/**
 * The game numbers the server simulates with. Generated from the operator's own install by
 * `npm run extract -- "<Fable folder>"` (see src/data/extract.ts), saved to
 * server/data/game-data.json, and never committed. When the file is missing the server
 * runs on FALLBACK_DATA, a small hand-made set so tests and dev work without a game install.
 *
 * Units follow Fable: health and damage are Fable's own numbers, prices are gold.
 */

export type WeaponType = 'sword' | 'axe' | 'hammer' | 'longbow' | 'crossbow' | 'other';
export type WeaponClass = 'light' | 'heavy' | 'ranged' | 'other';

export interface WeaponData {
  type: WeaponType;
  class: WeaponClass;
  /** Fable's base damage. The game's inventory screen shows this x10. */
  damage: number;
}

export interface ItemData {
  /** Fable's base value in gold (what a shop charges before its markup). */
  value: number;
  weapon?: WeaponData;
  /** Share of the body this piece of hero clothing covers (0..1). */
  coverage?: number;
  /** ARMOUR_* def this clothing counts as, e.g. ARMOUR_HERO_PLATINUM_ARMOUR. */
  armour?: string;
}

export interface CreatureData {
  health: number;
  /** General experience the hero earns for the kill. */
  experience: number;
  /** A second reward value read next to experience (thought to be gold or renown; unverified). */
  reward2: number;
  faction: string | null;
  combatType: string | null;
  /** The weapon def the creature fights with, if any. */
  weapon: string | null;
  /** Thing group, e.g. G_CREATURES_BANDIT. Used for loot tables and AI families. */
  group: string | null;
  /** A per-creature melee multiplier read from the creature def (unverified meaning). */
  attackScale: number;
  /** Fable's drop tables (REWARD_* object families) this creature rolls on when it dies. */
  rewards?: string[];
}

/** One row of a Fable drop table: an object def (null = nothing) and its weight. */
export interface RewardRow {
  item: string | null;
  weight: number;
}

export interface SpellData {
  /** Raw per-level arrays (4 levels) keyed by field hash, for later tuning work. */
  levels: Record<string, number[]>;
}

/** What a quest card (OBJECT_QUEST_CARD_*) says the quest pays. */
export interface QuestCardData {
  gold: number;
  renown: number;
  items: string[];
  /** Most the quest's boasts add on top (script.bin), when it has any. */
  boastGold?: number;
  boastRenown?: number;
}

export interface GameData {
  source: { kind: 'game.bin' | 'fallback'; namesCrc?: string; extractedAt?: string };
  items: Record<string, ItemData>;
  creatures: Record<string, CreatureData>;
  /** HERO_ABILITY_* names in Fable's order. */
  abilities: string[];
  spells: Record<string, SpellData>;
  /** CREATURE_GENERATION_FAMILY name -> the creature defs a generator may pick from it. */
  families: Record<string, string[]>;
  questCards: Record<string, QuestCardData>;
  /** OBJECT_FAMILY REWARD_* name -> its weighted rows (one pick per kill). */
  rewardTables?: Record<string, RewardRow[]>;
}

/** Hand-made numbers in Fable's ballpark. Not taken from the game files. */
export const FALLBACK_DATA: GameData = {
  source: { kind: 'fallback' },
  items: {
    OBJECT_IRON_LONGSWORD: { value: 200, weapon: { type: 'sword', class: 'light', damage: 4 } },
    OBJECT_IRON_WARHAMMER: { value: 500, weapon: { type: 'hammer', class: 'heavy', damage: 6 } },
    OBJECT_YEW_LONGBOW: { value: 250, weapon: { type: 'longbow', class: 'ranged', damage: 3 } },
    OBJECT_HERO_SHIRT_CHAINMAIL: { value: 2000, armour: 'ARMOUR_HERO_CHAINMAIL', coverage: 0.4 },
    OBJECT_HERO_HELMET_PLATINUM: { value: 1000, armour: 'ARMOUR_HERO_PLATINUM_ARMOUR', coverage: 0.1 },
    OBJECT_HEALTH_POTION: { value: 100 },
    OBJECT_WILL_POTION: { value: 250 },
    OBJECT_RESURRECTION_PHIAL: { value: 1000 },
    OBJECT_GEMSTONE_RUBY: { value: 600 },
  },
  creatures: {
    CREATURE_BANDIT_GRUNT_LEVEL1: { health: 30, experience: 10, reward2: 10, faction: 'FACTION_BANDITS', combatType: null, weapon: 'OBJECT_IRON_LONGSWORD', group: 'G_CREATURES_BANDIT', attackScale: 1, rewards: ['REWARD_BANDIT_01'] },
    CREATURE_EARTH_TROLL: { health: 200, experience: 120, reward2: 90, faction: 'FACTION_MONSTERS', combatType: null, weapon: null, group: 'G_CREATURES_FAE', attackScale: 3.5, rewards: ['REWARD_TROLL_01'] },
    CREATURE_WOLF: { health: 20, experience: 8, reward2: 0, faction: 'FACTION_MONSTERS', combatType: null, weapon: null, group: 'G_CREATURES_ANIMAL', attackScale: 1.5 },
    CREATURE_TRADER_TEST: { health: 20, experience: 0, reward2: 0, faction: 'FACTION_TRADERS', combatType: null, weapon: null, group: null, attackScale: 1 },
  },
  abilities: [],
  spells: {},
  families: { BANDITS_TEST: ['CREATURE_BANDIT_GRUNT_LEVEL1', 'CREATURE_BANDIT_GRUNT_LEVEL1'], WOLVES_TEST: ['CREATURE_WOLF'] },
  questCards: {
    OBJECT_QUEST_CARD_TEST_WOLVES: { gold: 500, renown: 100, items: ['OBJECT_HEALTH_POTION'] },
    OBJECT_QUEST_CARD_TEST_NO_GOLD: { gold: 0, renown: 50, items: [] },
  },
  // Same shape as Fable's own tables (weights as in game.bin).
  rewardTables: {
    REWARD_BANDIT_01: [
      { item: 'OBJECT_GOLD_5', weight: 150 },
      { item: 'OBJECT_GOLD_10', weight: 50 },
      { item: 'OBJECT_DYNAMIC_POTION_DUMMY', weight: 50 },
      { item: 'OBJECT_DYNAMIC_RESURRECTION_DUMMY', weight: 150 },
      { item: 'OBJECT_RED_MEAT_01', weight: 75 },
      { item: null, weight: 500 },
    ],
    REWARD_TROLL_01: [
      { item: 'OBJECT_DYNAMIC_POTION_DUMMY', weight: 100 },
      { item: 'OBJECT_GEMSTONE_RUBY', weight: 200 },
    ],
  },
};

export function loadGameData(file: string | undefined): GameData {
  if (!file || !fs.existsSync(file)) return FALLBACK_DATA;
  const d = JSON.parse(fs.readFileSync(file, 'utf8')) as GameData;
  if (!d.items || !d.creatures) throw new Error(`${file} is not a game-data file`);
  return { ...d, abilities: d.abilities ?? [], spells: d.spells ?? {}, families: d.families ?? {}, questCards: d.questCards ?? {}, rewardTables: d.rewardTables ?? {} };
}
