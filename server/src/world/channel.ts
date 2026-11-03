import type { RegionDef } from './regions.js';
import type { PlayerInput, RegionSim } from './sim.js';

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

  /** Advance the sim and send every player the world state. Entities are encoded once. */
  step(dtMs: number): void {
    if (this.loading.size) {
      const now = Date.now();
      for (const [id, until] of this.loading) if (now >= until) this.loading.delete(id);
    }
    this.sim.step(dtMs);
    this.tick++;
    if (this.players.size === 0) return;
    const entities = JSON.stringify(this.sim.snapshot());
    const head = `{"t":"tick","tick":${this.tick},"last_input_seq":`;
    for (const p of this.players.values()) p.send(`${head}${p.lastSeq},"entities":${entities}}`);
  }

  broadcast(frame: string): void {
    for (const p of this.players.values()) p.send(frame);
  }
}
