import crypto from 'node:crypto';
import type { GameData } from '../data/game-data.js';
import type { Creature } from '../world/entities.js';
import { GameError } from '../errors.js';
import { MINT_REASONS, type Economy } from './economy.js';

/**
 * What a creature drops, decided by the server when it dies. Gold is minted through the
 * ledger with reason 'loot' (capped per kill); items are minted as new unique items. A kill
 * can only pay once: the idempotency key is the creature's process-unique id plus a boot id.
 */
export interface LootDrop {
  defId: string;
  /** Chance per kill, 0..1. */
  chance: number;
}

export interface LootRules {
  /** Gold = creature reward value x this x a random 0.5..1. */
  goldPerReward: number;
  /** Item drops by thing group (G_CREATURES_BANDIT ...) and by exact creature def. */
  byGroup: Record<string, LootDrop[]>;
  byCreature: Record<string, LootDrop[]>;
}

export const DEFAULT_LOOT: LootRules = {
  goldPerReward: 1,
  byGroup: {
    G_CREATURES_BANDIT: [
      { defId: 'OBJECT_HEALTH_POTION', chance: 0.08 },
      { defId: 'OBJECT_APPLE', chance: 0.1 },
    ],
    G_CREATURES_FAE: [{ defId: 'OBJECT_HEALTH_POTION', chance: 0.05 }],
    G_CREATURES_UNDEAD: [{ defId: 'OBJECT_WILL_POTION', chance: 0.05 }],
  },
  byCreature: {},
};

/** Never dropped, whatever a loot table says. Phials stay scarce; staff gear is never minted by loot. */
const NEVER_DROPS = new Set(['OBJECT_RESURRECTION_PHIAL']);

export interface LootResult {
  gold: number;
  items: string[];
}

export class LootService {
  private boot = crypto.randomBytes(4).toString('hex');

  constructor(
    private economy: Economy,
    private data: GameData,
    private rules: LootRules = DEFAULT_LOOT,
    private rng: () => number = Math.random,
  ) {}

  roll(c: Creature): { gold: number; items: string[] } {
    const d = this.data.creatures[c.defName];
    const reward = d?.reward2 ?? 0;
    const gold = Math.min(MINT_REASONS.loot, Math.floor(reward * this.rules.goldPerReward * (0.5 + this.rng() * 0.5)));
    const drops = [...(this.rules.byCreature[c.defName] ?? []), ...(d?.group ? (this.rules.byGroup[d.group] ?? []) : [])];
    const items: string[] = [];
    for (const drop of drops) {
      if (NEVER_DROPS.has(drop.defId) || !this.economy.catalog.has(drop.defId)) continue;
      if (this.economy.catalog.get(drop.defId).staffOnly) continue;
      if (this.rng() < drop.chance) items.push(drop.defId);
    }
    return { gold, items };
  }

  /** Pay out a kill to the hero who earned it. Safe to call twice for the same kill. */
  award(heroId: number, c: Creature): LootResult {
    const { gold, items } = this.roll(c);
    const base = `loot:${this.boot}:${c.id}`;
    const out: LootResult = { gold: 0, items: [] };
    try {
      if (gold > 0) {
        const r = this.economy.grant(heroId, gold, 'loot', `${base}:gold`);
        if (!r.replayed) out.gold = gold;
      }
      items.forEach((defId, i) => {
        const r = this.economy.mintItem(heroId, defId, 1, 'loot', `${base}:item:${i}`);
        if (!r.replayed) out.items.push(r.itemId);
      });
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
    }
    return out;
  }
}
