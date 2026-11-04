import type { WorldClock } from '../clock.js';
import { CREATURE_ID_BASE } from '../entities.js';
import type { RegionDef } from '../regions.js';
import type { EntityState, PlayerInput, RegionSim, SimFactory, Vec3 } from '../sim.js';
import type { TownData } from './town-data.js';
import { NPC_ID_BASE, TownLife, type Ground, type TownOptions } from './townsfolk.js';

/** Input bits that count as a fight breaking out (same bits as combat-sim BUTTONS.ATTACK and CAST). */
const VIOLENT_BUTTONS = (1 << 0) | (1 << 2);
/**
 * Creature anim ids from here up are fight animations (attack, alert, hit, leap ... in the
 * monster AI's Anim table). A monster idling or walking near a trader camp doesn't empty it.
 */
const FIGHT_ANIM = 3;

export interface TownServices {
  data: TownData;
  clock: WorldClock;
  /** Walkable ground for a region (the navmesh), if one was extracted. */
  ground?: (region: RegionDef) => Ground | undefined;
  options?: Partial<TownOptions>;
  /** Guards posted at each village that has a guard def (CVillagePeopleDef's guard). */
  guardsPerVillage?: number;
}

/**
 * Adds town life to any region sim without touching it: the inner sim keeps heroes, combat and
 * monsters; this layer adds villagers and shopkeepers to the snapshot and lets them react to
 * what the inner sim is doing (monsters nearby, heroes swinging or casting).
 */
export class TownSim implements RegionSim {
  readonly town: TownLife;
  private heroes = new Map<number, { buttons: number }>();
  private last: EntityState[] = [];

  constructor(
    readonly inner: RegionSim,
    region: RegionDef,
    s: TownServices,
  ) {
    this.town = new TownLife(region.name, s.data, s.clock, s.ground?.(region), s.options);
    // The town watch: guards at every village, and the town folk as witnesses.
    if (inner.law && inner.addGuard) {
      const per = s.guardsPerVillage ?? 2;
      for (const v of s.data.regions[region.name]?.villages ?? []) {
        const def = s.data.villages[v.def]?.guard;
        if (!def) continue;
        for (let i = 0; i < per; i++) {
          const a = (i / per) * Math.PI * 2;
          inner.addGuard(def, { x: v.x + Math.cos(a) * 3, y: v.y + Math.sin(a) * 3, z: v.z });
        }
      }
      inner.law.addWitnessSource(() =>
        [...this.town.npcs.values()].map((n) => ({
          id: n.id,
          defName: n.def,
          kind: 'creature' as const,
          pos: n.pos,
          yaw: n.yaw,
          health: 1,
          maxHealth: 1,
          faction: 'villager',
          alive: n.state !== 'indoors',
          anim: n.anim,
          targetId: null,
          spawnId: null,
          attackCooldownMs: 0,
          blocking: false,
          stunnedMs: 0,
        })),
      );
    }
  }

  addHero(heroId: number, at: Vec3): void {
    this.heroes.set(heroId, { buttons: 0 });
    this.inner.addHero(heroId, at);
  }

  removeHero(heroId: number): Vec3 | undefined {
    this.heroes.delete(heroId);
    return this.inner.removeHero(heroId);
  }

  applyInput(heroId: number, input: PlayerInput): void {
    const h = this.heroes.get(heroId);
    if (h) h.buttons = input.buttons;
    this.inner.applyInput(heroId, input);
  }

  step(dtMs: number): void {
    this.inner.step(dtMs);
    this.last = this.inner.snapshot();
    const heroes: Vec3[] = [];
    const dangers: Vec3[] = [];
    for (const e of this.last) {
      if (e.id < CREATURE_ID_BASE) {
        heroes.push(e);
        if ((this.heroes.get(e.id)?.buttons ?? 0) & VIOLENT_BUTTONS) dangers.push(e);
      } else if (e.id < NPC_ID_BASE && e.health > 0 && e.anim >= FIGHT_ANIM) dangers.push(e); // a monster mid-fight
    }
    this.town.step(dtMs, heroes, dangers);
  }

  snapshot(): EntityState[] {
    return [...this.last, ...this.town.snapshot()];
  }

  position(heroId: number): Vec3 | undefined {
    return this.inner.position(heroId);
  }

  get law() {
    return this.inner.law;
  }

  get healing() {
    return this.inner.healing;
  }

  refreshLoadout(heroId: number): void {
    this.inner.refreshLoadout?.(heroId);
  }

  combatState(heroId: number) {
    return this.inner.combatState?.(heroId);
  }

  addGuard(defName: string, at: Vec3): number {
    if (!this.inner.addGuard) throw new Error('this region has no town watch');
    return this.inner.addGuard(defName, at);
  }

  placeHero(heroId: number, at: Vec3, yaw?: number): boolean {
    if (!this.inner.placeHero) return false;
    return this.inner.placeHero(heroId, at, yaw) !== false;
  }

  dispose(): void {
    this.town.dispose();
    this.inner.dispose();
  }
}

/** The town layer of a channel's sim, if it has one. */
export const townOf = (sim: RegionSim): TownLife | undefined => (sim instanceof TownSim ? sim.town : undefined);

/** Wrap a sim factory so regions with villages get town folk. Other regions are left as they are. */
export function withTown(inner: SimFactory, s: TownServices): SimFactory {
  return (region, channelId) => {
    const sim = inner(region, channelId);
    return s.data.regions[region.name] ? new TownSim(sim, region, s) : sim;
  };
}
