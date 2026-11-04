import type { Economy } from '../economy/economy.js';
import { GameError } from '../errors.js';
import type { SimHealing } from '../world/sim.js';

/**
 * Heals the player's own Fable did. Until Fable's item use and spell casting are mapped,
 * the mod only sees the hero's health go up and reports that, as a share of Fable's max.
 * The server never takes that number as health: it works out what the hero could have used
 * (a potion or food they carry, or Heal Life they can cast now), spends it, and heals by the
 * server's own amount. A heal that matches nothing is refused, and the mod puts the health
 * back to the server's on the next frame.
 */

export type HealSource = { source: 'spell'; share: number } | { source: 'item'; def: string; share: number };

/**
 * Share of max health each healing item gives back. Keys are the server's item ids
 * (the economy catalog ids, or the Fable def for items without one).
 */
export const HEAL_ITEMS: Record<string, number> = {
  HealthPotion: 0.4,
  OBJECT_SUPER_HEALTH_POTION: 1,
  OBJECT_APPLE: 0.05,
  OBJECT_CARROT: 0.05,
  OBJECT_MUSHROOM: 0.05,
  OBJECT_FISH: 0.1,
  OBJECT_RED_MEAT: 0.1,
  OBJECT_CHICKEN: 0.1,
  OBJECT_PIE: 0.15,
  OBJECT_TOFU: 0.1,
};

/** A reported heal this far from every option (as a ratio either way) matches nothing. */
export const HEAL_MATCH_RATIO = 2;

/**
 * The option whose heal is nearest (by ratio) to what the client saw, or null. When two are
 * as near, the first wins, so callers list the spell first: Will comes back on its own, items
 * do not.
 */
export function pickHeal(reported: number, options: HealSource[]): HealSource | null {
  if (!(reported > 0) || !Number.isFinite(reported)) return null;
  let best: HealSource | null = null;
  let bestErr = Math.log(HEAL_MATCH_RATIO);
  for (const o of options) {
    if (!(o.share > 0)) continue;
    const err = Math.abs(Math.log(o.share / reported));
    if (err <= bestErr + 1e-9 && (best === null || err < bestErr - 1e-9)) {
      best = o;
      bestErr = err;
    }
  }
  return best;
}

export interface HealResult {
  ok: boolean;
  replayed?: boolean;
  source?: 'spell' | 'item';
  def?: string;
  /** Health the server added, in its own units. */
  healed?: number;
  reason?: string;
}

/**
 * The whole decision for one reported heal. `key` is the server-side idempotency key; a
 * resend with the same key spends nothing again.
 */
export function claimHeal(healing: SimHealing | undefined, economy: Economy, heroId: number, share: number, key: string): HealResult {
  const st = healing?.state(heroId);
  if (!healing || !st) return { ok: false, reason: 'no_combat' };
  if (!st.alive) return { ok: false, reason: 'dead' };
  if (economy.claimed(key)) return { ok: true, replayed: true };
  const options: HealSource[] = [];
  if (st.spellHeal !== null) options.push({ source: 'spell', share: st.spellHeal / st.maxHealth });
  // Only what the hero carries: a potion in the bank cannot have been drunk.
  const carried = new Set(economy.inventory(heroId).filter((i) => i.location === 'inventory' && i.quantity > 0).map((i) => i.def_id));
  for (const [def, s] of Object.entries(HEAL_ITEMS)) if (carried.has(def)) options.push({ source: 'item', def, share: s });
  const pick = pickHeal(share, options);
  if (!pick) return { ok: false, reason: options.length ? 'no_match' : 'nothing_to_heal_with' };
  if (pick.source === 'spell') {
    // Will and cooldown are the spell's cost; it has no ledger entry, so a resend can only
    // cast again if the hero could cast again anyway.
    if (!healing.castHealLife(heroId)) return { ok: false, reason: 'cannot_cast' };
    return { ok: true, source: 'spell', healed: (healing.state(heroId)?.health ?? st.health) - st.health };
  }
  try {
    economy.consumeOne(heroId, pick.def, key);
  } catch (e) {
    if (e instanceof GameError) return { ok: false, reason: e.code };
    throw e;
  }
  return { ok: true, source: 'item', def: pick.def, healed: healing.addHealth(heroId, pick.share * st.maxHealth) };
}
