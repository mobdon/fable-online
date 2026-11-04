import crypto from 'node:crypto';
import { isBound, type ItemDef } from '../../economy/catalog.js';

/**
 * What a container entry from the level files turns into on the server.
 *
 *  - OBJECT_GOLD_<n> and OBJECT_GOLDBAG_..._<n>: n gold, minted with reason "loot".
 *  - OBJECT_DYNAMIC_*_DUMMY: Fable rolls these at run time; the server rolls them instead,
 *    seeded by hero and thing so a retry gives the same result.
 *  - Anything matching BLOCKED is never created from world loot. The platinum hero set is
 *    staff only; one chest in the Northern Wastes holds the whole set in the retail data.
 *  - Everything else becomes one item of that Fable def id.
 */
export type LootEntry = { gold: number } | { item: string; quantity: number } | { blocked: string };

export const BLOCKED: RegExp[] = [/^OBJECT_HERO_[A-Z]+_PLATINUM$/];

/** Starter catalog ids that already exist for some Fable defs. */
export const ALIASES: Record<string, string> = {
  OBJECT_HEALTH_POTION: 'HealthPotion',
  OBJECT_WILL_POTION: 'WillPotion',
  OBJECT_MANA_POTION: 'WillPotion',
  OBJECT_RESURRECTION_PHIAL: 'ResurrectionPhial',
};

const DYNAMIC: Record<string, string[]> = {
  OBJECT_DYNAMIC_POTION_DUMMY: ['OBJECT_HEALTH_POTION', 'OBJECT_HEALTH_POTION', 'OBJECT_MANA_POTION', 'OBJECT_MANA_POTION', 'OBJECT_SUPER_HEALTH_POTION'],
  // Phials stay scarce: the dynamic phial slot rolls a phial one time in four.
  OBJECT_DYNAMIC_RESURRECTION_DUMMY: ['OBJECT_RESURRECTION_PHIAL', 'OBJECT_HEALTH_POTION', 'OBJECT_HEALTH_POTION', 'OBJECT_MANA_POTION'],
};

/** Gold a single loot action may create. Mirrors MINT_REASONS.loot in the economy. */
export const MAX_LOOT_GOLD = 5_000;

export function resolveLoot(contents: string[], seed: string): LootEntry[] {
  const out: LootEntry[] = [];
  contents.forEach((raw, i) => {
    let def = raw;
    const roll = DYNAMIC[def];
    if (roll) def = roll[pick(`${seed}:${i}`, roll.length)]!;
    if (BLOCKED.some((r) => r.test(def))) return out.push({ blocked: def });
    const gold = def.match(/^OBJECT_GOLD(?:BAG_[A-Z_]*?)?_(\d+)$/);
    if (gold) return out.push({ gold: Number(gold[1]) });
    out.push({ item: ALIASES[def] ?? def, quantity: 1 });
  });
  return out;
}

/**
 * Catalog entry for a world-loot item the starter catalog doesn't know yet. Vendors
 * won't buy or sell it until real prices come from the game data, so it can't be turned
 * into gold; consumables stack.
 */
export function lootItemDef(id: string): ItemDef {
  const stack = /POTION|APPLE|PIE|MEAT|CARROT|TOFU|FISH|CHICK|MUSHROOM|AUGMENTATION|GEMSTONE|KEY$/.test(id);
  return { id, maxStack: stack ? 99 : 1, buy: null, sell: null, tradeable: !isBound(id) };
}

function pick(seed: string, n: number): number {
  return crypto.createHash('sha256').update(seed).digest().readUInt32LE(0) % n;
}
