import type { RegionDef } from './regions.js';
import type { PlayerInput, RegionSim, Vec3, WireEntity } from './sim.js';

/** A connected player as the world sees it. `send` takes an already-encoded frame. */
export interface PlayerLink {
  heroId: number;
  name: string;
  renown: number;
  send(frame: string): void;
  /** Highest input seq the sim has applied; echoed back so the client can reconcile. */
  lastSeq: number;
  joinedAt: number;
}

export type ChannelKind = 'shared' | 'private';

/**
 * One copy of one region. Shared channels hold up to the hard cap of players; private
 * instances hold exactly one hero during childhood and Guild training.
 */
export class Channel {
  readonly players = new Map<number, PlayerLink>();
  readonly openedAt = Date.now();
  emptySince: number | null = Date.now();
  tick = 0;
  /**
   * Heroes who just arrived through a region change and are still loading the region in
   * their own game. Their inputs are ignored until the client says it is ready (or the
   * deadline passes), so nobody walks around blind on the server side.
   */
  readonly loading = new Map<number, number>();

  constructor(
    readonly id: string,
    readonly region: RegionDef,
    readonly kind: ChannelKind,
    readonly sim: RegionSim,
    readonly ownerHeroId?: number,
    /**
     * Entities further than this from a player are left out of that player's frames.
     * A region can hold hundreds of creatures, and the mod can only show what is near
     * enough to see. 0 sends everything (and lets every player share one encoded frame).
     */
    readonly interestRadius = 0,
  ) {}

  get count(): number {
    return this.players.size;
  }

  add(p: PlayerLink, at: { x: number; y: number; z: number }): void {
    this.players.set(p.heroId, p);
    this.sim.addHero(p.heroId, at);
    this.emptySince = null;
  }

  remove(heroId: number) {
    const pos = this.sim.removeHero(heroId);
    this.players.delete(heroId);
    this.loading.delete(heroId);
    if (this.players.size === 0) this.emptySince = Date.now();
    return pos;
  }

  input(heroId: number, input: PlayerInput): void {
    const p = this.players.get(heroId);
    if (!p || input.seq <= p.lastSeq) return; // stale or replayed frame
    p.lastSeq = input.seq;
    if (this.loading.has(heroId)) return;
    this.sim.applyInput(heroId, input);
  }

  /** Dev only: trust the mod's own idea of where its hero is. See RegionSim.placeHero. */
  placeHero(heroId: number, at: Vec3, yaw?: number): boolean {
    if (!this.players.has(heroId) || !this.sim.placeHero || this.loading.has(heroId)) return false;
    this.sim.placeHero(heroId, at, yaw);
    return true;
  }

  /**
   * Everything in the channel as the wire sees it: the sim's entities, with the players'
   * names and renown attached so the mod can label their stand-ins.
   */
  entities(): WireEntity[] {
    return this.sim.snapshot().map((e) => {
      const p = this.players.get(e.id);
      if (p) return { ...e, kind: 'hero' as const, name: p.name, renown: p.renown };
      return { ...e, kind: 'creature' as const };
    });
  }

  /** Advance the sim and send every player the world state. */
  step(dtMs: number): void {
    if (this.loading.size) {
      const now = Date.now();
      for (const [id, until] of this.loading) if (now >= until) this.loading.delete(id);
    }
    this.sim.step(dtMs);
    this.tick++;
    if (this.players.size === 0) return;
    const all = this.entities();
    const head = `{"t":"tick","tick":${this.tick},"last_input_seq":`;

    // Without an interest radius every player sees the same thing, so encode it once.
    if (this.interestRadius <= 0) {
      const encoded = JSON.stringify(all);
      for (const p of this.players.values()) p.send(`${head}${p.lastSeq},"entities":${encoded}}`);
      return;
    }

    const r2 = this.interestRadius * this.interestRadius;
    for (const p of this.players.values()) {
      const me = this.sim.position(p.heroId);
      // A player whose own position is unknown gets everything rather than nothing.
      const near = me
        ? all.filter((e) => e.id === p.heroId || (e.x - me.x) ** 2 + (e.y - me.y) ** 2 <= r2)
        : all;
      p.send(`${head}${p.lastSeq},"entities":${JSON.stringify(near)}}`);
    }
  }

  broadcast(frame: string): void {
    for (const p of this.players.values()) p.send(frame);
  }
}
