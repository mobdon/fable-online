import type { GameData } from '../data/game-data.js';

/**
 * The weapons the hero has equipped in their own Fable, as the mod reads them
 * (client-mod/src/game/gear.cpp). Quests and shops still run in each player's game, so a
 * hero's sword is often one the server's ledger has never seen; without this the server
 * fights with bare hands (BALANCE.unarmedDamage).
 *
 * A report is a claim, not state. A weapon counts only when:
 *  - it is a known item with weapon stats in game-data,
 *  - it is not staff-only gear, unless the hero is staff,
 *  - the hero's ledger holds it, or the realm trusts client gear (FO_TRUST_CLIENT_GEAR=1,
 *    test realms only, while Fable's own inventory is not synced to the ledger).
 * Anything else is refused and the hero keeps fighting with what the ledger says.
 */
export interface GearReport {
  /** Def name of the equipped melee weapon, '' for none. */
  melee: string;
  /** Def name of the equipped ranged weapon, '' for none. */
  ranged: string;
}

export type GearRefusal = 'unknown_item' | 'not_a_weapon' | 'wrong_slot' | 'staff_only' | 'not_owned';

export interface GearResult {
  /** True when what the server fights with changed. */
  changed: boolean;
  accepted: string[];
  refused: { def: string; reason: GearRefusal }[];
}

export interface GearRules {
  data: GameData;
  /** Accept any known weapon the client names (test realms). */
  trust: boolean;
  owns(heroId: number, defId: string): boolean;
  staffOnly(defId: string): boolean;
  isStaff(heroId: number): boolean;
}

export class ClientGear {
  private byHero = new Map<number, string[]>();

  constructor(private rules: GearRules) {}

  report(heroId: number, r: GearReport): GearResult {
    const accepted: string[] = [];
    const refused: GearResult['refused'] = [];
    const slots: [string, 'melee' | 'ranged'][] = [
      [r.melee, 'melee'],
      [r.ranged, 'ranged'],
    ];
    for (const [def, slot] of slots) {
      if (!def) continue;
      const why = this.check(heroId, def, slot);
      if (why) refused.push({ def, reason: why });
      else accepted.push(def);
    }
    const before = this.byHero.get(heroId) ?? [];
    const changed = before.length !== accepted.length || before.some((d, i) => d !== accepted[i]);
    if (accepted.length) this.byHero.set(heroId, accepted);
    else this.byHero.delete(heroId);
    return { changed, accepted, refused };
  }

  /** Accepted weapons, melee first: heroCombatant fights with the first weapon it finds. */
  equipped(heroId: number): string[] {
    return this.byHero.get(heroId) ?? [];
  }

  forget(heroId: number): void {
    this.byHero.delete(heroId);
  }

  private check(heroId: number, def: string, slot: 'melee' | 'ranged'): GearRefusal | null {
    const { data, trust } = this.rules;
    const item = data.items[def];
    if (!item) return 'unknown_item';
    if (!item.weapon) return 'not_a_weapon';
    if ((item.weapon.class === 'ranged') !== (slot === 'ranged')) return 'wrong_slot';
    if (this.rules.staffOnly(def) && !this.rules.isStaff(heroId)) return 'staff_only';
    if (!trust && !this.rules.owns(heroId, def)) return 'not_owned';
    return null;
  }
}
