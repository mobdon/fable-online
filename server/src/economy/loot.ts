import crypto from 'node:crypto';
import type { GameData, RewardRow } from '../data/game-data.js';
import type { Creature } from '../world/entities.js';
import { GameError } from '../errors.js';
import { MINT_REASONS, type Economy } from './economy.js';

/**
 * What a creature drops, decided by the server when it dies. Gold only: nothing a kill gives
 * can be resold or hoarded, and Resurrection Phials never drop. Gold is minted through the
 * ledger with reason 'loot'.
 *
 * mode 'reward' (the default): every kill pays the creature's own reward value from game.bin
 * (CCreatureDef reward2: bandit grunt level 1 = 6, hobbe grunt = 10, earth troll = 90).
 *
 * mode 'tables': Fable's own drop tables (REWARD_* object families) decide, per kill, in order:
 *  1. Tables with gold rows (OBJECT_GOLD_<n>): each table pays with `goldChance`, the amount
 *     picked by Fable's relative weights between its gold rows, times `goldScale`.
 *  2. Else tables with gems (trolls, nymphs): Fable's own gem chance, paid as a share of
 *     the gem's base value instead of the gem.
 *  3. Else (no table, or one with nothing in it): the creature's reward value (reward2)
 *     with `fallbackChance`, times 0.5..1.
 *
 * A kill can only pay once: the idempotency key is the creature's process-unique id plus a
 * boot id. Each hero can earn at most `hourlyGoldCap` loot gold per clock hour.
 */
export interface LootDrop {
  defId: string;
  /** Chance per kill, 0..1. */
  chance: number;
}

export interface LootRules {
  /** 'reward': every kill pays the creature's reward value. 'tables': Fable's drop tables (below). */
  mode: 'reward' | 'tables';
  /** Chance a table with gold rows pays out at all (Fable's own is about 0.2). */
  goldChance: number;
  /** Multiplier on Fable's gold amounts. */
  goldScale: number;
  /** Share of a gem's base value paid as gold when a gem row comes up. */
  gemShare: number;
  /** Chance a creature with no gold or gems in its table pays its reward value. */
  fallbackChance: number;
  /** Most loot gold one hero can earn per clock hour. 0 = no cap. */
  hourlyGoldCap: number;
  /** Optional item drops by thing group (G_CREATURES_BANDIT ...) and by exact creature def. Empty by default. */
  byGroup: Record<string, LootDrop[]>;
  byCreature: Record<string, LootDrop[]>;
}

export const DEFAULT_LOOT: LootRules = {
  mode: 'reward',
  goldChance: 0.5,
  goldScale: 2,
  gemShare: 0.25,
  fallbackChance: 0.5,
  hourlyGoldCap: 3_000,
  byGroup: {},
  byCreature: {},
};

/** Never dropped, whatever a loot table says. Phials stay scarce; staff gear is never minted by loot. */
const NEVER_DROPS = new Set(['OBJECT_RESURRECTION_PHIAL']);

const GOLD_ROW = /^OBJECT_GOLD_(\d+)$/;
const GEM_ROW = /^OBJECT_GEMSTONE_/;

export interface LootResult {
  gold: number;
  items: string[];
}

const HOUR_MS = 3_600_000;

export class LootService {
  private boot = crypto.randomBytes(4).toString('hex');
  private earned = new Map<number, { hour: number; gold: number }>();
  readonly rules: LootRules;

  constructor(
    private economy: Economy,
    private data: GameData,
    rules: Partial<LootRules> = {},
    private rng: () => number = Math.random,
    private now: () => number = Date.now,
  ) {
    this.rules = { ...DEFAULT_LOOT, ...rules };
  }

  roll(c: Creature): { gold: number; items: string[] } {
    const d = this.data.creatures[c.defName];
    const tables = (d?.rewards ?? []).map((n) => this.data.rewardTables?.[n]).filter((t): t is RewardRow[] => !!t?.length);
    const reward = d?.reward2 ?? 0;
    const gold = Math.min(MINT_REASONS.loot, this.rules.mode === 'reward' ? reward : this.rollGold(tables, reward));
    const drops = [...(this.rules.byCreature[c.defName] ?? []), ...(d?.group ? (this.rules.byGroup[d.group] ?? []) : [])];
    const items: string[] = [];
    for (const drop of drops) {
      if (NEVER_DROPS.has(drop.defId) || !this.economy.catalog.has(drop.defId)) continue;
      if (this.economy.catalog.get(drop.defId).staffOnly) continue;
      if (this.rng() < drop.chance) items.push(drop.defId);
    }
    return { gold, items };
  }

  private rollGold(tables: RewardRow[][], reward2: number): number {
    const r = this.rules;
    const goldTables = tables.map((t) => t.filter((row) => GOLD_ROW.test(row.item ?? ''))).filter((g) => g.length);
    if (goldTables.length) {
      let gold = 0;
      for (const rows of goldTables) {
        if (this.rng() >= r.goldChance) continue;
        const row = this.pick(rows);
        gold += Math.floor(Number(GOLD_ROW.exec(row.item!)![1]) * r.goldScale);
      }
      return gold;
    }
    const gemTables = tables.filter((t) => t.some((row) => GEM_ROW.test(row.item ?? '')));
    if (gemTables.length) {
      let gold = 0;
      for (const t of gemTables) {
        const gems = t.filter((row) => GEM_ROW.test(row.item ?? ''));
        const chance = sum(gems) / sum(t);
        if (this.rng() >= chance) continue;
        const gem = this.pick(gems).item!;
        gold += Math.floor((this.data.items[gem]?.value ?? 0) * r.gemShare);
      }
      return gold;
    }
    if (reward2 <= 0 || this.rng() >= r.fallbackChance) return 0;
    return Math.floor(reward2 * (0.5 + this.rng() * 0.5));
  }

  private pick(rows: RewardRow[]): RewardRow {
    let x = this.rng() * sum(rows);
    for (const row of rows) if ((x -= row.weight) < 0) return row;
    return rows[rows.length - 1]!;
  }

  /** Loot gold the hero can still earn this clock hour. */
  remaining(heroId: number): number {
    if (this.rules.hourlyGoldCap <= 0) return Infinity;
    const e = this.earned.get(heroId);
    const hour = Math.floor(this.now() / HOUR_MS);
    return !e || e.hour !== hour ? this.rules.hourlyGoldCap : Math.max(0, this.rules.hourlyGoldCap - e.gold);
  }

  /** Pay out a kill to the hero who earned it. Safe to call twice for the same kill. */
  award(heroId: number, c: Creature): LootResult {
    const rolled = this.roll(c);
    const gold = Math.min(rolled.gold, this.remaining(heroId));
    const base = `loot:${this.boot}:${c.id}`;
    const out: LootResult = { gold: 0, items: [] };
    try {
      if (gold > 0) {
        const r = this.economy.grant(heroId, gold, 'loot', `${base}:gold`);
        if (!r.replayed) {
          out.gold = gold;
          this.count(heroId, gold);
        }
      }
      rolled.items.forEach((defId, i) => {
        const r = this.economy.mintItem(heroId, defId, 1, 'loot', `${base}:item:${i}`);
        if (!r.replayed) out.items.push(r.itemId);
      });
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
    }
    return out;
  }

  private count(heroId: number, gold: number): void {
    const hour = Math.floor(this.now() / HOUR_MS);
    const e = this.earned.get(heroId);
    if (!e || e.hour !== hour) this.earned.set(heroId, { hour, gold });
    else e.gold += gold;
    if (this.earned.size > 10_000) for (const [id, v] of this.earned) if (v.hour !== hour) this.earned.delete(id);
  }
}

const sum = (rows: RewardRow[]) => rows.reduce((a, r) => a + r.weight, 0);
