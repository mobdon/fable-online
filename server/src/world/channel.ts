import type { RegionDef } from './regions.js';
import type { PlayerInput, RegionSim, Vec3, WireEntity } from './sim.js';
import { MOVE_RULES, MoveGuard, type MoveRules, type MoveVerdict } from './movement.js';

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
  /** Position report checks per hero, for heroes moved by the mod's own reports. */
  readonly moves = new Map<number, MoveGuard>();
  /**
   * Where this region's frame starts on Albion's map (RegionTable.origin). The sim works in
   * the level data's own frame; the wire, like Fable, uses world coordinates.
   */
  origin: { x: number; y: number } = { x: 0, y: 0 };

  /** A position from the client (world) in the sim's frame. */
  toLocal<T extends { x: number; y: number }>(v: T): T {
    return { ...v, x: v.x - this.origin.x, y: v.y - this.origin.y };
  }

  /** A sim position as the client sees it (world). */
  toWorld<T extends { x: number; y: number }>(v: T): T {
    return { ...v, x: v.x + this.origin.x, y: v.y + this.origin.y };
  }
  /** Last sky state sent to this channel (Director.syncSky). */
  skyKey = '';
  skyAt = 0;

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
    readonly moveRules: MoveRules = MOVE_RULES,
  ) {}

  get count(): number {
    return this.players.size;
  }

  /**
   * `anchor` lets the hero's first position report through from up to that many metres from
   * `at` (Infinity: anywhere), because the client's Fable may have put the hero somewhere else.
   */
  add(p: PlayerLink, at: { x: number; y: number; z: number }, anchor = 0): void {
    this.players.set(p.heroId, p);
    this.sim.addHero(p.heroId, at);
    this.moves.set(p.heroId, new MoveGuard(Date.now(), anchor, this.moveRules));
    this.emptySince = null;
  }

  remove(heroId: number) {
    const pos = this.sim.removeHero(heroId);
    this.players.delete(heroId);
    this.loading.delete(heroId);
    this.moves.delete(heroId);
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

  /** Trust the mod's own idea of where its hero is, unchecked. Dev only: see reportPosition. */
  placeHero(heroId: number, at: Vec3, yaw?: number): boolean {
    if (!this.players.has(heroId) || !this.sim.placeHero || this.loading.has(heroId)) return false;
    const from = this.sim.position(heroId);
    if (this.sim.placeHero(heroId, at, yaw) === false) return false;
    // Unchecked moves still feed the guard's speed, so a trusted hero's stand-in walks and runs too.
    if (from) this.moves.get(heroId)?.observe(from, at, Date.now());
    return true;
  }

  /**
   * The mod reported where its Fable has the hero (`at` in world coordinates, as Fable has
   * them). Accepted only if the hero could have got
   * there from where the server has them (MoveGuard). On refusal the hero stays put and the
   * caller gets the server's position to send back, at most every correctEveryMs.
   */
  reportPosition(
    heroId: number,
    at: Vec3,
    yaw: number | undefined,
    now = Date.now(),
  ): { verdict: MoveVerdict | { ok: false; reason: 'loading' | 'not_here' | 'refused' }; correct?: Vec3 & { yaw?: number } } {
    const g = this.moves.get(heroId);
    const server = this.sim.position(heroId);
    if (!g || !server || !this.players.has(heroId) || !this.sim.placeHero) return { verdict: { ok: false, reason: 'not_here' } };
    // While loading the next region the client's coordinates are still the last region's.
    if (this.loading.has(heroId)) return { verdict: { ok: false, reason: 'loading' } };
    const local = this.toLocal(at);
    const verdict = g.check(server, local, now);
    if (verdict.ok && this.sim.placeHero(heroId, local, yaw) !== false) return { verdict };
    const why = verdict.ok ? { ok: false as const, reason: 'refused' as const } : verdict;
    return { verdict: why, correct: g.mayCorrect(now) ? this.toWorld(server) : undefined };
  }

  /**
   * Everything in the channel as the wire sees it: the sim's entities, with the players'
   * names and renown attached so the mod can label their stand-ins.
   */
  entities(): WireEntity[] {
    const now = Date.now();
    return this.sim.snapshot().map((e) => {
      const p = this.players.get(e.id);
      if (p) {
        // A hero moved by position reports has no stick input, so the sim thinks they stand
        // still: take walk or run from how fast the reports moved them instead.
        const g = this.moves.get(e.id);
        const anim = g && e.anim <= 1 && e.health > 0 ? Math.max(e.anim, g.anim(now)) : e.anim;
        return { ...e, anim, kind: 'hero' as const, name: p.name, renown: p.renown };
      }
      return { ...e, kind: 'creature' as const };
    });
  }

  private wire(es: WireEntity[]): WireEntity[] {
    const { x, y } = this.origin;
    return x === 0 && y === 0 ? es : es.map((e) => ({ ...e, x: e.x + x, y: e.y + y }));
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

    // Without an interest radius, and with everyone in the same area, every player sees the
    // same thing, so encode it once.
    if (this.interestRadius <= 0 && this.oneArea()) {
      const encoded = JSON.stringify(this.wire(all));
      for (const p of this.players.values()) p.send(`${head}${p.lastSeq},"entities":${encoded}}`);
      return;
    }

    const r2 = this.interestRadius * this.interestRadius;
    for (const p of this.players.values()) {
      const me = this.sim.position(p.heroId);
      const myArea = this.areaOf(p.heroId);
      // A player whose own position is unknown gets everything rather than nothing.
      const near = all.filter(
        (e) =>
          e.id === p.heroId ||
          (this.sameArea(myArea, e) && (!me || r2 <= 0 || (e.x - me.x) ** 2 + (e.y - me.y) ** 2 <= r2)),
      );
      p.send(`${head}${p.lastSeq},"entities":${JSON.stringify(this.wire(near))}}`);
    }
  }

  /** The client's own area hash for a hero, 0 if it has not said. */
  areaOf(heroId: number): number {
    return this.moves.get(heroId)?.area ?? 0;
  }

  /**
   * Until the server follows every exit, heroes whose Fable loaded a different area stay in
   * this channel. Their coordinates belong to another map, so their stand-ins are hidden
   * from each other. Creatures and unknown areas are always shown.
   */
  private sameArea(myArea: number, e: WireEntity): boolean {
    if (!myArea || e.kind !== 'hero') return true;
    const theirs = this.areaOf(e.id);
    return !theirs || theirs === myArea;
  }

  private oneArea(): boolean {
    let seen = 0;
    for (const id of this.players.keys()) {
      const a = this.areaOf(id);
      if (!a) continue;
      if (seen && a !== seen) return false;
      seen = a;
    }
    return true;
  }

  broadcast(frame: string): void {
    for (const p of this.players.values()) p.send(frame);
  }
}
