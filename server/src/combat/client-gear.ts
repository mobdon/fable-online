import type { GameData } from '../data/game-data.js';

/**
 * The weapons and clothing the hero has equipped in their own Fable, as the mod reads them
 * (client-mod/src/game/gear.cpp). Quests and shops still run in each player's game, so a
 * hero's sword is often one the server's ledger has never seen; without this the server
 * fights with bare hands (BALANCE.unarmedDamage).
 *
 * A report is a claim, not state. A piece counts only when:
 *  - it is a known item with weapon stats (weapons) or body coverage (clothing) in game-data,
 *  - it is not staff-only gear, unless the hero is staff,
 *  - the hero's ledger holds it, or the realm trusts client gear (FO_TRUST_CLIENT_GEAR=1,
 *    test realms only, while Fable's own inventory is not synced to the ledger).
 * Anything else is refused and the hero keeps fighting with what the ledger says.
 *
 * Clothing: once a report carries an armour list (even an empty one), the ledger's equipped
 * clothing no longer counts; what the hero wears in Fable is what protects them. A report
 * without one (the mod couldn't read it) leaves the ledger's clothing in place.
 */
export interface GearReport {
  /** Def name of the equipped melee weapon, '' for none. */
  melee: string;
  /** Def name of the equipped ranged weapon, '' for none. */
  ranged: string;
  /** Def names of the clothing worn; absent when the mod could not read it. */
  armour?: string[];
}

export type GearRefusal = 'unknown_item' | 'not_a_weapon' | 'not_armour' | 'wrong_slot' | 'staff_only' | 'not_owned';

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

interface Accepted {
  weapons: string[];
  /** null: no armour list reported, the ledger's clothing stands. */
  armour: string[] | null;
}

export class ClientGear {
  private byHero = new Map<number, Accepted>();

  constructor(private rules: GearRules) {}

  report(heroId: number, r: GearReport): GearResult {
    const refused: GearResult['refused'] = [];
    const weapons: string[] = [];
    const slots: [string, 'melee' | 'ranged'][] = [
      [r.melee, 'melee'],
      [r.ranged, 'ranged'],
    ];
    for (const [def, slot] of slots) {
      if (!def) continue;
      const why = this.check(heroId, def, slot);
      if (why) refused.push({ def, reason: why });
      else weapons.push(def);
    }
    let armour: string[] | null = null;
    if (r.armour) {
      armour = [];
      for (const def of new Set(r.armour)) {
        const why = this.check(heroId, def, 'armour');
        if (why) refused.push({ def, reason: why });
        else armour.push(def);
      }
    }
    const next: Accepted | undefined = weapons.length || armour ? { weapons, armour } : undefined;
    const changed = key(this.byHero.get(heroId)) !== key(next);
    if (next) this.byHero.set(heroId, next);
    else this.byHero.delete(heroId);
    return { changed, accepted: [...weapons, ...(armour ?? [])], refused };
  }

  /** Accepted weapons (melee first) and clothing. heroCombatant fights with the first weapon it finds. */
  equipped(heroId: number): string[] {
    const a = this.byHero.get(heroId);
    return a ? [...a.weapons, ...(a.armour ?? [])] : [];
  }

  /**
   * The combat loadout: accepted gear first, then the ledger's equipped items, without the
   * ledger's clothing once Fable's clothing has been reported, and without repeats.
   */
  loadout(heroId: number, ledgerEquipped: string[]): string[] {
    const a = this.byHero.get(heroId);
    const mine = this.equipped(heroId);
    const seen = new Set(mine);
    const rest = ledgerEquipped.filter((d) => {
      if (seen.has(d)) return false;
      if (a?.armour && this.rules.data.items[d]?.coverage) return false;
      seen.add(d);
      return true;
    });
    return [...mine, ...rest];
  }

  forget(heroId: number): void {
    this.byHero.delete(heroId);
  }

  private check(heroId: number, def: string, slot: 'melee' | 'ranged' | 'armour'): GearRefusal | null {
    const { data, trust } = this.rules;
    const item = data.items[def];
    if (!item) return 'unknown_item';
    if (slot === 'armour') {
      if (!item.coverage) return 'not_armour';
    } else {
      if (!item.weapon) return 'not_a_weapon';
      if ((item.weapon.class === 'ranged') !== (slot === 'ranged')) return 'wrong_slot';
    }
    if (this.rules.staffOnly(def) && !this.rules.isStaff(heroId)) return 'staff_only';
    if (!trust && !this.rules.owns(heroId, def)) return 'not_owned';
    return null;
  }
}

const key = (a: Accepted | undefined) => (a ? JSON.stringify([a.weapons, a.armour]) : '');
