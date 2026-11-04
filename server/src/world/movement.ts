import type { Vec3 } from './sim.js';

/**
 * Checks on the mod's own hero position reports (`hero_pos`), used until Fable's input is
 * mapped and the server can move heroes from intent alone.
 *
 * The client says where its Fable put the hero; the server only believes it if the hero
 * could have got there from where the *server* has them, in the time since the last report.
 * Distance is paid for from a budget that refills at the top speed and holds about a second
 * of it, so a roll or a late packet is fine but a speed hack or a teleport is not. A refused
 * report leaves the hero where the server has them, and the client is told so it snaps back.
 */
export const MOVE_RULES = {
  /** Metres per second along the ground. Fable's sprint is about 7; rolls and slopes need headroom. */
  maxSpeed: 9,
  /** Seconds of top speed the budget can hold (absorbs jitter, a lag spike, a roll). */
  burstSeconds: 1.2,
  /** Metres the hero may rise in one report, plus this much per second (stairs, slopes, a jump). */
  climb: 2.5,
  climbPerSecond: 4,
  /** Metres per second the hero may drop (a fall off a ledge). */
  fallSpeed: 30,
  /** Coordinates past this are junk whatever the region. */
  bound: 16_384,
  /**
   * A hero who just arrived through a region change appears at the entrance the server chose,
   * and the client's Fable at its own idea of that entrance: this much difference is let through once.
   */
  arrivalSlack: 40,
  /** Fewest milliseconds between two corrections sent to the same client. */
  correctEveryMs: 500,
  /**
   * Fewest milliseconds between two area changes the client loads by itself. A real load
   * takes seconds, so anything faster is a client faking area changes to teleport.
   */
  areaChangeEveryMs: 3_000,
  /**
   * A hero refused for a jump who then keeps moving normally from the new spot for this long
   * is taken to be there: Fable loaded another area by itself and the mod could not tell.
   * Until the server follows exits this is what lets a hero leave a town. 0 turns it off.
   */
  settleMs: 2_000,
  /** Metres a settling hero may drift from one refused report to the next, plus the speed budget. */
  settleSlack: 3,
  /** Ground speed thresholds for the stand-in's animation (metres per second). */
  walkFrom: 0.4,
  runFrom: 4,
} as const;

export type MoveRules = typeof MOVE_RULES;

/** What the sim's anim field means for heroes, matching client-mod/src/world/snapshot.h. */
export const ANIM = { IDLE: 0, WALK: 1, RUN: 2 } as const;

export type MoveVerdict =
  | { ok: true; anchored: boolean; speed: number }
  | { ok: false; reason: 'too_fast' | 'too_high' | 'too_low' | 'out_of_bounds' };

/**
 * One hero's report checker. `anchor` lets the next report through from anywhere within that
 * many metres (Infinity: anywhere in the region), for the first report after a join or arrival.
 */
export class MoveGuard {
  private budget: number;
  private lastAt: number;
  private anchor: number;
  /** Smoothed ground speed from accepted reports, for the stand-in's animation. */
  speed = 0;
  lastCorrectionAt = -Infinity;
  rejected = 0;
  /** The client's own area hash from its last report (0: not known yet). */
  area = 0;
  private lastAreaChangeAt = -Infinity;
  private refusedArea = 0;
  /** Where refused reports have been coming from, and since when (see MOVE_RULES.settleMs). */
  private pending: { since: number; at: Vec3 } | null = null;

  constructor(
    now: number,
    anchor = 0,
    private rules: MoveRules = MOVE_RULES,
  ) {
    this.lastAt = now;
    this.anchor = anchor;
    this.budget = rules.maxSpeed * rules.burstSeconds;
  }

  /** Allow one report from up to `radius` metres away (a region arrival, a server teleport). */
  reanchor(radius: number): void {
    this.anchor = Math.max(this.anchor, radius);
  }

  /**
   * The client's Fable loaded another area on its own (the hero walked out of the town, into
   * a house, through a Cullis Gate) before the server knows the exits. The coordinates jump
   * to the new area's, so the next report is let through from anywhere, once. Returns true
   * when that happened. Too many changes too quickly are not believed.
   */
  noteArea(area: number, now: number): 'same' | 'changed' | 'too_soon' {
    if (!area) return 'same';
    const before = this.area;
    if (before === area) return 'same';
    // The first area a client tells us about is where it already is.
    if (!before) {
      this.area = area;
      return 'same';
    }
    // Not believed yet: keep the old area so the change is tried again once the wait is over.
    if (now - this.lastAreaChangeAt < this.rules.areaChangeEveryMs) {
      if (this.refusedArea === area) return 'same';
      this.refusedArea = area;
      return 'too_soon';
    }
    this.area = area;
    this.refusedArea = 0;
    this.lastAreaChangeAt = now;
    this.reanchor(Infinity);
    this.speed = 0;
    return 'changed';
  }

  /**
   * The mod saw its Fable stall the way it does while loading an area (or rebuild the hero).
   * Same as an area change, for when the area hash does not move.
   */
  noteLoad(now: number): 'changed' | 'too_soon' {
    if (now - this.lastAreaChangeAt < this.rules.areaChangeEveryMs) return 'too_soon';
    this.lastAreaChangeAt = now;
    this.reanchor(Infinity);
    this.speed = 0;
    return 'changed';
  }

  private settled(next: Vec3, now: number, dt: number): boolean {
    const r = this.rules;
    if (r.settleMs <= 0) return false;
    const p = this.pending;
    if (p && Math.hypot(next.x - p.at.x, next.y - p.at.y, next.z - p.at.z) <= r.settleSlack + r.maxSpeed * Math.max(dt, 0.1)) {
      p.at = next;
      if (now - p.since < r.settleMs || now - this.lastAreaChangeAt < r.areaChangeEveryMs) return false;
      this.pending = null;
      this.lastAreaChangeAt = now;
      this.speed = 0;
      this.rejected = 0;
      return true;
    }
    this.pending = { since: now, at: next };
    return false;
  }

  get anchored(): boolean {
    return this.anchor > 0;
  }

  check(server: Vec3, next: Vec3, now: number): MoveVerdict {
    const r = this.rules;
    if (Math.abs(next.x) > r.bound || Math.abs(next.y) > r.bound || Math.abs(next.z) > r.bound) return { ok: false, reason: 'out_of_bounds' };
    const dt = Math.min(Math.max(now - this.lastAt, 0), 5_000) / 1000;
    this.lastAt = now;
    this.budget = Math.min(this.budget + r.maxSpeed * dt, r.maxSpeed * r.burstSeconds);
    const ground = Math.hypot(next.x - server.x, next.y - server.y);
    const rise = next.z - server.z;

    if (this.anchor > 0 && Math.hypot(ground, rise) <= this.anchor) {
      this.anchor = 0;
      this.speed = 0;
      return { ok: true, anchored: true, speed: 0 };
    }
    if (ground > this.budget) {
      if (this.settled(next, now, dt)) return { ok: true, anchored: true, speed: 0 };
      return this.refuse('too_fast');
    }
    this.pending = null;
    if (rise > r.climb + r.climbPerSecond * dt) return this.refuse('too_high');
    if (-rise > r.climb + r.fallSpeed * dt) return this.refuse('too_low');

    this.budget -= ground;
    this.rejected = 0;
    const instant = dt > 0 ? ground / dt : 0;
    // Smooth over a few reports so one short step does not flick the stand-in between walk and idle.
    this.speed = this.speed * 0.5 + instant * 0.5;
    return { ok: true, anchored: false, speed: this.speed };
  }

  /** Track speed for a move that was not checked (clientMovement 'trusted'). */
  observe(from: Vec3, to: Vec3, now: number): void {
    const dt = Math.max(now - this.lastAt, 1) / 1000;
    this.lastAt = now;
    this.anchor = 0;
    this.speed = this.speed * 0.5 + (Math.hypot(to.x - from.x, to.y - from.y) / dt) * 0.5;
  }

  /** The animation a stand-in should play for this hero's reported movement. */
  anim(now: number): number {
    // The mod stops reporting while the hero stands still, so silence means idle.
    if (now - this.lastAt > 400) return ANIM.IDLE;
    if (this.speed >= this.rules.runFrom) return ANIM.RUN;
    if (this.speed >= this.rules.walkFrom) return ANIM.WALK;
    return ANIM.IDLE;
  }

  /** True when a correction may be sent now (and records that it was). */
  mayCorrect(now: number): boolean {
    if (now - this.lastCorrectionAt < this.rules.correctEveryMs) return false;
    this.lastCorrectionAt = now;
    return true;
  }

  private refuse(reason: 'too_fast' | 'too_high' | 'too_low'): MoveVerdict {
    this.rejected++;
    this.speed = 0;
    return { ok: false, reason };
  }
}
