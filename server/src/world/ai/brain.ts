import { seededRng, type Point2, type Rng } from '../nav/navmesh.js';
import type { Vec3 } from '../sim.js';
import { profileFor, type AiProfile } from './profiles.js';
import { Anim, type AiHooks, type AiWorld, type AttackKind, type Combatant, type Creature } from './types.js';

export type AiState = 'idle' | 'wander' | 'alert' | 'chase' | 'attack' | 'strafe' | 'flee' | 'return' | 'approach' | 'dead';

/** How often a creature re-thinks. Movement still runs every tick. */
const THINK_MS = 200;
/** A target out of sight this long is forgotten. */
const LOSE_TRACK_MS = 4000;
/** A new target must beat the current one by this factor (stops ping-ponging between heroes). */
const SWITCH_FACTOR = 1.3;
const ARRIVE = 0.3;

interface Brain {
  c: Creature;
  p: AiProfile;
  state: AiState;
  home: Vec3;
  stateUntil: number;
  nextThink: number;
  /** Waypoints still to walk, and the point they lead to. */
  path: Point2[];
  goal: Point2 | null;
  speed: number;
  moveAnim: number;
  repathAt: number;
  /** Hero id -> threat (damage dealt, plus being noticed). */
  threat: Map<number, number>;
  lastSeen: Map<number, number>;
  fled: boolean;
  alliesLost: number;
  leapReadyAt: number;
  leapUntil: number;
  blockUntil: number;
  strafeDir: 1 | -1;
  /** 'approach' only: who to walk up to, and how close to stop. */
  approachId: number | null;
  approachStop: number;
  /** Got there: hold the spot (just keep facing) until approach() is called again. */
  approachArrived: boolean;
  /** A swing being wound up: when it lands (0 = none), what kind, at whom. */
  swingAt: number;
  swingStart: number;
  swingKind: AttackKind;
  swingTarget: number | null;
  /** No new swing before this. */
  recoverUntil: number;
  /** Swings left in the current flurry. */
  comboLeft: number;
  /** The strike pose is held until this. */
  strikeUntil: number;
  /** Going home because it was dragged off its patch (heals on arrival), not because the fight ended. */
  leashed: boolean;
  /** A beat to walk instead of wandering (town guards), with the next point and direction. */
  patrol: { route: Vec3[]; at: number; dir: 1 | -1 } | null;
}

/**
 * The shortest wind-up any profile uses (profiles.ts windupMs): the clients start the strike
 * animation when the swing starts, so the hit always lands at least this long after it.
 */
export const SWING_WINDUP_MS = 300;

/** How long the strike pose is held after a swing lands, so the next swing reads as a new one. */
const FOLLOW_THROUGH_MS = 300;
/**
 * Like Fable, only a couple of creatures swing at one hero at a time; the rest of the
 * group circles close by, waiting for an opening, so a crowd doesn't land every hit at once.
 */
const MAX_SWINGING = 2;

/**
 * Runs every creature in one channel. Decides where creatures go and when they swing;
 * combat decides what a swing does. No Fable.exe involved: the ground comes from the
 * navmesh, and behaviour is a small state machine per creature:
 *
 *   idle <-> wander --notice--> alert --> chase <-> attack (<-> strafe)
 *                                           |          |
 *                                           +--> flee -+--> return --> idle (healed)
 *
 * Leash: a creature dragged further than profile.leash from its spawn point stops
 * fighting, runs home ignoring damage, and heals to full. That closes the "pull it to
 * town and kite it" exploits. A creature whose fight simply ended (its target died or got
 * away) walks home keeping its wounds.
 *
 * Melee follows Fable's rhythm: a visible wind-up before each swing (a flinch cancels it,
 * and the hit only lands if the target is still in reach and in front), a flurry of one or
 * two swings, then a pause spent circling with the guard up.
 */
export class AiDirector implements AiHooks {
  private brains = new Map<number, Brain>();
  private now = 0;
  private rng: Rng;

  constructor(
    private world: AiWorld,
    seed = 1,
  ) {
    this.rng = seededRng(seed);
  }

  // ---------- hooks from spawns and combat ----------

  onSpawn(c: Creature): void {
    const p = profileFor(c);
    const b: Brain = {
      c,
      p,
      state: 'idle',
      home: { ...c.pos },
      stateUntil: this.now + this.between(p.idleMs),
      nextThink: this.now + this.rng() * THINK_MS, // stagger thinking across ticks
      path: [],
      goal: null,
      speed: 0,
      moveAnim: Anim.Idle,
      repathAt: 0,
      threat: new Map(),
      lastSeen: new Map(),
      fled: false,
      alliesLost: 0,
      leapReadyAt: 0,
      leapUntil: 0,
      blockUntil: 0,
      strafeDir: this.rng() < 0.5 ? 1 : -1,
      approachId: null,
      approachStop: 2,
      approachArrived: false,
      swingAt: 0,
      swingStart: 0,
      swingKind: 'light',
      swingTarget: null,
      recoverUntil: 0,
      comboLeft: 0,
      strikeUntil: 0,
      leashed: false,
      patrol: null,
    };
    c.anim = Anim.Idle;
    this.brains.set(c.id, b);
  }

  onDamaged(creatureId: number, attackerId: number, amount: number): void {
    const b = this.brains.get(creatureId);
    if (!b || b.state === 'dead' || b.state === 'return') return; // evading creatures shrug it off
    b.threat.set(attackerId, (b.threat.get(attackerId) ?? 0) + Math.max(1, amount) * 2);
    b.lastSeen.set(attackerId, this.now);
    if (b.state === 'idle' || b.state === 'wander' || b.state === 'alert' || b.state === 'approach') {
      const t = this.findTarget(attackerId);
      if (t) {
        this.engage(b, t, false);
        this.callPack(b, t);
      }
    }
    if (b.state !== 'flee') this.checkMorale(b);
  }

  onDeath(creatureId: number): void {
    const b = this.brains.get(creatureId);
    if (!b) return;
    b.state = 'dead';
    b.swingAt = 0;
    b.c.anim = Anim.Death;
    b.c.targetId = null;
    b.path = [];
    b.goal = null;
    for (const o of this.brains.values()) {
      if (o === b || o.state === 'dead' || o.p.family !== b.p.family) continue;
      if (dist2(o.c.pos, b.c.pos) > 20 * 20) continue;
      if (o.c.targetId !== null) {
        o.alliesLost++;
        this.checkMorale(o);
      }
    }
  }

  /** Forget a creature (despawned or removed). */
  remove(creatureId: number): void {
    this.brains.delete(creatureId);
  }

  // ---------- orders from other server systems (the law module's guards) ----------

  /** Chase and fight this target now, skipping the alert pause, and keep it as top threat. */
  pursue(creatureId: number, targetId: number): boolean {
    const b = this.brains.get(creatureId);
    const t = this.findTarget(targetId);
    if (!b || b.state === 'dead' || !t || !t.alive) return false;
    b.threat.set(t.id, Math.max(b.threat.get(t.id) ?? 0, 1000));
    if (b.c.targetId === t.id && (b.state === 'chase' || b.state === 'attack' || b.state === 'strafe')) return true;
    this.engage(b, t, false);
    return true;
  }

  /** Walk up to a target and stand facing it without attacking (a guard asking for a fine). */
  approach(creatureId: number, targetId: number, stopAt = 2): boolean {
    const b = this.brains.get(creatureId);
    const t = this.findTarget(targetId);
    if (!b || b.state === 'dead' || !t || !t.alive) return false;
    if (b.state === 'approach' && b.approachId === targetId) {
      b.approachStop = stopAt;
      b.approachArrived = false;
      return true;
    }
    if (b.c.blocking) this.world.combat.setBlocking(b.c.id, false);
    b.state = 'approach';
    b.approachId = targetId;
    b.approachStop = stopAt;
    b.approachArrived = false;
    b.c.targetId = targetId;
    b.nextThink = this.now;
    return true;
  }

  /** Drop every target and go back to the post (fine paid, bribe taken, hero arrested). */
  standDown(creatureId: number): void {
    const b = this.brains.get(creatureId);
    if (!b || b.state === 'dead') return;
    b.threat.clear();
    b.lastSeen.clear();
    b.approachId = null;
    this.goHome(b);
  }

  /**
   * Walk this beat when not busy: point to point, then back the way it came (Fable's guard
   * tracks are open chains). The creature's home follows it along the beat, so its leash and
   * the walk back after a fight are measured from where it was on its round.
   */
  setPatrol(creatureId: number, route: Vec3[]): void {
    const b = this.brains.get(creatureId);
    if (!b) return;
    if (route.length < 2) {
      b.patrol = null;
      return;
    }
    // Start at the point nearest to where it stands.
    let at = 0;
    for (let i = 1; i < route.length; i++) if (dist(b.c.pos, route[i]!) < dist(b.c.pos, route[at]!)) at = i;
    b.patrol = { route: route.map((p) => ({ ...p })), at, dir: at === route.length - 1 ? -1 : 1 };
    b.home = { ...route[at]! };
  }

  state(creatureId: number): AiState | undefined {
    return this.brains.get(creatureId)?.state;
  }

  home(creatureId: number): Vec3 | undefined {
    return this.brains.get(creatureId)?.home;
  }

  // ---------- tick ----------

  step(dtMs: number): void {
    this.now += dtMs;
    for (const b of this.brains.values()) {
      if (b.state === 'dead') continue;
      if (!b.c.alive) {
        this.onDeath(b.c.id);
        continue;
      }
      if (b.blockUntil && this.now >= b.blockUntil) {
        b.blockUntil = 0;
        this.world.combat.setBlocking(b.c.id, false);
      }
      if (b.c.stunnedMs > 0) {
        // Staggered: combat owns the animation, and a swing being wound up is lost.
        if (b.swingAt) this.cancelSwing(b);
        continue;
      }
      if (b.swingAt && this.now >= b.swingAt) this.land(b);
      else if (b.swingAt) {
        // Track the target through the first part of the wind-up, then commit to the line.
        const t = b.swingTarget === null ? undefined : this.findTarget(b.swingTarget);
        if (t && this.now - b.swingStart < (b.swingAt - b.swingStart) * 0.6) this.face(b, t.pos);
      } else if (b.c.anim === Anim.Attack && this.now >= b.strikeUntil && b.state === 'attack') {
        b.c.anim = Anim.Alert; // swing over: back to the guard stance, so the next swing reads as new
      }
      if (this.now >= b.nextThink) {
        b.nextThink = this.now + THINK_MS;
        this.think(b);
      }
      this.move(b, dtMs);
    }
  }

  // ---------- decisions ----------

  private think(b: Brain): void {
    switch (b.state) {
      case 'idle':
      case 'wander': {
        const t = this.perceive(b);
        if (t) {
          this.engage(b, t, true);
          this.callPack(b, t);
          return;
        }
        if (b.patrol) return this.patrolStep(b);
        if (b.state === 'idle' && this.now >= b.stateUntil) {
          const nav = this.world.nav;
          const p = nav ? nav.randomPointNear(b.home, b.p.wanderRadius, this.rng) : jitter(b.home, b.p.wanderRadius, this.rng);
          if (p) {
            b.state = 'wander';
            this.goTo(b, p, b.p.walkSpeed, Anim.Walk);
          } else b.stateUntil = this.now + this.between(b.p.idleMs);
        } else if (b.state === 'wander' && !b.goal) {
          b.state = 'idle';
          b.stateUntil = this.now + this.between(b.p.idleMs);
        }
        return;
      }
      case 'alert': {
        const t = this.currentTarget(b);
        if (!t) return this.disengage(b);
        this.face(b, t.pos);
        if (this.now >= b.stateUntil) b.state = 'chase';
        return;
      }
      case 'chase':
      case 'attack':
      case 'strafe':
        return this.fight(b);
      case 'flee': {
        const t = this.currentTarget(b);
        if (this.now >= b.stateUntil) {
          // Cornered (the hero kept up): turn and fight. Otherwise slink home.
          if (t && dist(b.c.pos, t.pos) < 6) b.state = 'chase';
          else this.goHome(b);
          return;
        }
        if (!b.goal && t) this.runFrom(b, t.pos);
        return;
      }
      case 'approach': {
        const t = b.approachId === null ? undefined : this.findTarget(b.approachId);
        if (!t || !t.alive) {
          b.approachId = null;
          return this.goHome(b);
        }
        if (dist(b.c.pos, b.home) > b.p.leash) {
          b.approachId = null;
          return this.goHome(b, true);
        }
        const d = dist(b.c.pos, t.pos);
        if (b.approachArrived) {
          this.face(b, t.pos);
        } else if (d <= b.approachStop) {
          this.stop(b);
          this.face(b, t.pos);
          b.c.anim = Anim.Idle;
          b.approachArrived = true;
        } else if (!b.goal || dist(b.goal, t.pos) > 1 || this.now >= b.repathAt) this.goTo(b, t.pos, b.p.runSpeed * 0.75, Anim.Run);
        return;
      }
      case 'return':
        if (!b.goal) {
          if (dist(b.c.pos, b.home) > 1.5) this.goTo(b, b.home, b.p.runSpeed, Anim.Run);
          else {
            if (b.leashed) b.c.health = b.c.maxHealth;
            b.leashed = false;
            b.threat.clear();
            b.lastSeen.clear();
            b.fled = false;
            b.alliesLost = 0;
            b.state = 'idle';
            b.c.anim = Anim.Idle;
            b.stateUntil = this.now + this.between(b.p.idleMs);
          }
        }
        return;
      case 'dead':
        return;
    }
  }

  /** Idle or walking its beat: on to the next point, with a short look round at each end. */
  private patrolStep(b: Brain): void {
    const pt = b.patrol!;
    if (b.state === 'wander' && b.goal) return;
    if (b.state === 'idle' && this.now < b.stateUntil) return;
    const atEnd = pt.at === 0 || pt.at === pt.route.length - 1;
    if (b.state === 'wander' && atEnd && dist(b.c.pos, pt.route[pt.at]!) < 1.5) {
      // Reached the end of the beat: stand a moment, then turn back.
      b.state = 'idle';
      b.c.anim = Anim.Idle;
      b.stateUntil = this.now + this.between(b.p.idleMs) / 2;
      pt.dir = pt.at === 0 ? 1 : -1;
      return;
    }
    if (dist(b.c.pos, pt.route[pt.at]!) < 1.5) {
      if (pt.at + pt.dir < 0 || pt.at + pt.dir >= pt.route.length) pt.dir = pt.dir === 1 ? -1 : 1;
      pt.at += pt.dir;
    }
    const next = pt.route[pt.at]!;
    b.home = { ...next };
    b.state = 'wander';
    this.goTo(b, next, b.p.walkSpeed, Anim.Walk);
    if (!b.goal) {
      // No way there on the ground: walk it straight rather than freeze.
      this.goTo(b, next, b.p.walkSpeed, Anim.Walk, true);
    }
  }

  private fight(b: Brain): void {
    const p = b.p;
    if (dist(b.c.pos, b.home) > p.leash) return this.goHome(b, true);
    if (b.swingAt) return; // mid wind-up: committed
    this.refreshThreat(b);
    const t = this.pickTarget(b);
    if (!t) return this.disengage(b);
    b.c.targetId = t.id;
    if (this.checkMorale(b)) return;

    const d = dist(b.c.pos, t.pos);
    const nav = this.world.nav;
    const canHit = d <= p.attackRange && (p.preferredRange === 0 || !nav || nav.clearLine(b.c.pos, t.pos));

    if (b.state === 'strafe') {
      if (this.now < b.stateUntil && d < p.attackRange + 4) {
        this.circle(b, t.pos);
        this.maybeBlock(b, t);
        return;
      }
      b.state = 'chase';
    }

    if (p.preferredRange > 0 && d < p.preferredRange - 4) {
      // Archer with a hero in its face: back off to its preferred distance.
      const away = this.awayPoint(b.c.pos, t.pos, p.preferredRange - d + 1);
      if (away) {
        b.state = 'chase';
        this.goTo(b, away, p.walkSpeed * 1.5, Anim.Walk);
        this.face(b, t.pos);
        return;
      }
    }

    if (canHit) {
      b.state = 'attack';
      this.stop(b);
      this.face(b, t.pos);
      if (b.c.attackCooldownMs <= 0 && this.now >= b.recoverUntil && !b.blockUntil) {
        if (p.preferredRange === 0 && this.swingingAt(t.id, b) >= MAX_SWINGING) {
          // Wait its turn: step round the hero with its guard up.
          b.state = 'strafe';
          b.stateUntil = this.now + 600 + this.rng() * 600;
          b.recoverUntil = b.stateUntil;
          return;
        }
        if (b.comboLeft <= 0) b.comboLeft = 1 + Math.floor(this.rng() * Math.max(1, p.comboMax));
        this.windUp(b, t);
      } else if (!b.c.blocking && b.c.anim !== Anim.Attack) {
        if (!this.maybeBlock(b, t) && b.c.anim !== Anim.Block) b.c.anim = Anim.Alert;
      }
      return;
    }

    b.state = 'chase';
    if (p.leap && d >= p.leap.min && d <= p.leap.max && this.now >= b.leapReadyAt && (!nav || nav.clearLine(b.c.pos, t.pos))) {
      b.leapReadyAt = this.now + p.leap.cooldownMs;
      b.leapUntil = this.now + (d / p.leap.speed) * 1000;
      this.goTo(b, this.slot(b, t.pos), p.leap.speed, Anim.Leap, true);
      return;
    }
    if (this.now < b.leapUntil) return; // mid-air
    // Re-path when the target has moved off the end of our path, or every second.
    const dest = this.slot(b, t.pos);
    if (!b.goal || dist(b.goal, dest) > 1.5 || this.now >= b.repathAt) this.goTo(b, dest, p.runSpeed, Anim.Run);
  }

  /** Other melee creatures winding up or mid-flurry against this target. */
  private swingingAt(targetId: number, self: Brain): number {
    let n = 0;
    for (const o of this.brains.values())
      if (o !== self && o.p.preferredRange === 0 && o.c.targetId === targetId && (o.swingAt || o.comboLeft > 0)) n++;
    return n;
  }

  /** Start a swing: the strike animation plays now, the hit is decided when it lands. */
  private windUp(b: Brain, t: Combatant): void {
    if (b.c.blocking) this.world.combat.setBlocking(b.c.id, false);
    b.blockUntil = 0;
    const kind = this.pickAttack(b.p);
    b.swingKind = kind;
    b.swingTarget = t.id;
    b.swingStart = this.now;
    b.swingAt = this.now + (b.p.windupMs[kind] ?? b.p.windupMs.light ?? 450);
    b.c.anim = Anim.Attack;
  }

  /** The swing lands: combat checks reach, facing and blocks at this moment. */
  private land(b: Brain): void {
    const target = b.swingTarget;
    const kind = b.swingKind;
    b.swingAt = 0;
    b.swingTarget = null;
    b.comboLeft = Math.max(0, b.comboLeft - 1);
    b.strikeUntil = this.now + FOLLOW_THROUGH_MS;
    if (target !== null) this.world.combat.attack(b.c.id, target, kind);
    if (b.comboLeft > 0) {
      // The next swing of the flurry follows the follow-through.
      b.recoverUntil = this.now + FOLLOW_THROUGH_MS + 100;
      return;
    }
    b.recoverUntil = this.now + this.between(b.p.recoverMs);
    if (b.p.circles && b.p.preferredRange === 0) {
      b.state = 'strafe';
      b.stateUntil = b.recoverUntil;
      if (this.rng() < 0.4) b.strafeDir = b.strafeDir === 1 ? -1 : 1;
    }
  }

  private cancelSwing(b: Brain): void {
    b.swingAt = 0;
    b.swingTarget = null;
    b.comboLeft = 0;
    // Knocked out of its swing: it needs a moment before trying again.
    b.recoverUntil = Math.max(b.recoverUntil, this.now + 400);
  }

  /** Between swings: raise the guard now and then, and more often when the target is winding up. */
  private maybeBlock(b: Brain, t: Combatant): boolean {
    const p = b.p;
    if (b.c.blocking || b.blockUntil || dist(b.c.pos, t.pos) > p.attackRange + 2.5) return false;
    const windingUp = ((t as { chargeMs?: number }).chargeMs ?? 0) > 0;
    const chance = windingUp ? Math.max(p.reactBlockChance, p.blockChance) : p.blockChance * 0.5;
    if (chance <= 0 || this.rng() >= chance) return false;
    this.world.combat.setBlocking(b.c.id, true);
    b.c.anim = Anim.Block;
    b.blockUntil = this.now + 600 + this.rng() * 500;
    return true;
  }

  /** Notice a target: a short alert pause (or none when hit) and then the chase. */
  private engage(b: Brain, t: Combatant, pause: boolean): void {
    b.threat.set(t.id, Math.max(b.threat.get(t.id) ?? 0, 1));
    b.lastSeen.set(t.id, this.now);
    b.c.targetId = t.id;
    this.stop(b);
    this.face(b, t.pos);
    if (pause && b.p.alertMs > 0) {
      b.state = 'alert';
      b.c.anim = Anim.Alert;
      b.stateUntil = this.now + b.p.alertMs;
    } else b.state = 'chase';
  }

  private callPack(b: Brain, t: Combatant): void {
    const r2 = b.p.packRadius * b.p.packRadius;
    for (const o of this.brains.values()) {
      if (o === b || o.p.family !== b.p.family) continue;
      if (o.state !== 'idle' && o.state !== 'wander') continue;
      if (dist2(o.c.pos, b.c.pos) > r2) continue;
      this.engage(o, t, true);
      o.stateUntil = this.now + o.p.alertMs * (0.5 + this.rng()); // don't all charge on the same frame
    }
  }

  /** Flee once at low health, or when too many pack-mates died. Returns true if it ran. */
  private checkMorale(b: Brain): boolean {
    const p = b.p;
    const low = p.fleeAt > 0 && !b.fled && b.c.health > 0 && b.c.health / b.c.maxHealth <= p.fleeAt;
    const broken = p.breaksAfterDeaths > 0 && !b.fled && b.alliesLost >= p.breaksAfterDeaths;
    if (!low && !broken) return false;
    b.fled = true;
    b.state = 'flee';
    b.stateUntil = this.now + p.fleeMs;
    if (b.c.blocking) this.world.combat.setBlocking(b.c.id, false);
    const t = this.currentTarget(b);
    if (t) this.runFrom(b, t.pos);
    else this.goHome(b);
    return true;
  }

  private disengage(b: Brain): void {
    b.c.targetId = null;
    this.goHome(b);
  }

  private goHome(b: Brain, leashed = false): void {
    b.state = 'return';
    b.leashed = b.leashed || leashed;
    b.swingAt = 0;
    b.swingTarget = null;
    b.comboLeft = 0;
    b.approachId = null;
    b.c.targetId = null;
    if (b.c.blocking) this.world.combat.setBlocking(b.c.id, false);
    this.goTo(b, b.home, b.p.runSpeed, Anim.Run);
  }

  // ---------- targets ----------

  private hostile(b: Brain, t: Combatant): boolean {
    if (!t.alive) return false;
    return this.world.isHostile ? this.world.isHostile(b.c, t) : t.faction !== b.c.faction;
  }

  /** The nearest hostile it can see or hear, if any, that is inside its patch. */
  private perceive(b: Brain): Combatant | undefined {
    const p = b.p;
    let best: Combatant | undefined;
    let bestD = Infinity;
    const halfFov = ((p.fov / 2) * Math.PI) / 180;
    for (const t of this.world.targets()) {
      if (!this.hostile(b, t)) continue;
      const d = dist(b.c.pos, t.pos);
      if (d > p.sight || d >= bestD) continue;
      if (dist(b.home, t.pos) > p.leash) continue;
      if (d > p.hear) {
        const ang = Math.abs(wrap(yawTo(b.c.pos, t.pos) - b.c.yaw));
        if (ang > halfFov) continue;
      }
      best = t;
      bestD = d;
    }
    return best;
  }

  private findTarget(id: number): Combatant | undefined {
    for (const t of this.world.targets()) if (t.id === id) return t;
    return undefined;
  }

  private currentTarget(b: Brain): Combatant | undefined {
    if (b.c.targetId === null) return undefined;
    const t = this.findTarget(b.c.targetId);
    return t && t.alive ? t : undefined;
  }

  /** Add whoever it can see, forget whoever it hasn't seen for a while. */
  private refreshThreat(b: Brain): void {
    const p = b.p;
    for (const t of this.world.targets()) {
      if (!this.hostile(b, t)) continue;
      const d = dist(b.c.pos, t.pos);
      if (d <= p.sight * 1.5) {
        b.lastSeen.set(t.id, this.now);
        if (d <= p.sight && !b.threat.has(t.id)) b.threat.set(t.id, 1);
      }
    }
    for (const [id, seen] of b.lastSeen) {
      if (this.now - seen > LOSE_TRACK_MS) {
        b.lastSeen.delete(id);
        b.threat.delete(id);
      }
    }
  }

  /** Highest threat plus closeness wins, but only by a clear margin over the current target. */
  private pickTarget(b: Brain): Combatant | undefined {
    const score = (t: Combatant) => (b.threat.get(t.id) ?? 0) + Math.max(0, b.p.sight - dist(b.c.pos, t.pos)) * 0.5;
    let best: Combatant | undefined;
    let bestS = -Infinity;
    let cur: Combatant | undefined;
    for (const t of this.world.targets()) {
      if (!b.threat.has(t.id) || !this.hostile(b, t)) continue;
      if (dist(b.home, t.pos) > b.p.leash + b.p.sight) continue;
      const s = score(t);
      if (t.id === b.c.targetId) cur = t;
      if (s > bestS) {
        bestS = s;
        best = t;
      }
    }
    if (cur && best !== cur && bestS <= score(cur) * SWITCH_FACTOR + 2) return cur;
    return best;
  }

  private pickAttack(p: AiProfile): AttackKind {
    const entries = Object.entries(p.attacks) as [AttackKind, number][];
    const total = entries.reduce((s, [, w]) => s + w, 0);
    let r = this.rng() * total;
    for (const [k, w] of entries) if ((r -= w) <= 0) return k;
    return entries[0]![0];
  }

  // ---------- movement ----------

  /**
   * Where to stand to hit the target: a spot on a ring around it, picked by creature id,
   * so a pack surrounds a hero instead of stacking on one point.
   */
  private slot(b: Brain, target: Point2): Point2 {
    if (b.p.preferredRange > 0) return target;
    const ang = (b.c.id * 2.399963) % (Math.PI * 2); // golden angle spreads ids evenly
    const r = b.p.attackRange * 0.75;
    const p = { x: target.x + Math.cos(ang) * r, y: target.y + Math.sin(ang) * r };
    return this.world.nav && !this.world.nav.walkable(p.x, p.y) ? target : p;
  }

  private circle(b: Brain, target: Point2): void {
    const r = b.p.attackRange + 1.5;
    const ang = Math.atan2(b.c.pos.y - target.y, b.c.pos.x - target.x) + b.strafeDir * 0.7;
    const p = { x: target.x + Math.cos(ang) * r, y: target.y + Math.sin(ang) * r };
    if (this.world.nav && !this.world.nav.walkable(p.x, p.y)) {
      b.strafeDir = b.strafeDir === 1 ? -1 : 1;
      return;
    }
    this.goTo(b, p, b.p.walkSpeed * 1.6, Anim.Strafe, true);
    this.face(b, target);
  }

  private runFrom(b: Brain, from: Point2): void {
    const p = this.awayPoint(b.c.pos, from, 14);
    if (p) this.goTo(b, p, b.p.runSpeed, Anim.Run);
  }

  /** A walkable point roughly `d` metres further from `threat`, best of a few tries. */
  private awayPoint(pos: Point2, threat: Point2, d: number): Point2 | undefined {
    const nav = this.world.nav;
    const base = Math.atan2(pos.y - threat.y, pos.x - threat.x);
    let best: Point2 | undefined;
    let bestD = -1;
    for (let i = 0; i < 6; i++) {
      const ang = base + (this.rng() - 0.5) * 1.6;
      const p = { x: pos.x + Math.cos(ang) * d, y: pos.y + Math.sin(ang) * d };
      if (nav && !nav.findPath(pos, p)) continue;
      const q = nav ? (nav.cellAt(p.x, p.y) ? p : nav.snap(p, 3)) : p;
      if (!q) continue;
      const dd = dist(q, threat);
      if (dd > bestD) {
        bestD = dd;
        best = q;
      }
    }
    return best;
  }

  private goTo(b: Brain, p: Point2, speed: number, anim: number, straight = false): void {
    const nav = this.world.nav;
    b.goal = { x: p.x, y: p.y };
    b.speed = speed;
    b.moveAnim = anim;
    b.repathAt = this.now + 1000;
    if (!nav || straight) {
      b.path = [b.goal];
      return;
    }
    const path = nav.findPath(b.c.pos, p);
    if (!path && !nav.nearestCell(b.c.pos.x, b.c.pos.y, 2)) {
      // Off the ground data (a bridge or ledge the level files don't cover): walk straight
      // rather than freeze until it is back on known ground.
      b.path = [b.goal];
      return;
    }
    if (!path) {
      b.path = [];
      b.goal = null;
      return;
    }
    b.path = path;
  }

  private stop(b: Brain): void {
    b.path = [];
    b.goal = null;
    b.speed = 0;
  }

  private face(b: Brain, p: Point2): void {
    if (dist(b.c.pos, p) > 0.01) b.c.yaw = yawTo(b.c.pos, p);
  }

  private move(b: Brain, dtMs: number): void {
    const c = b.c;
    if (!b.goal || b.path.length === 0) {
      b.goal = null;
      if (c.anim === Anim.Walk || c.anim === Anim.Run || c.anim === Anim.Strafe || c.anim === Anim.Leap) c.anim = Anim.Idle;
      return;
    }
    const nav = this.world.nav;
    let left = (b.speed * dtMs) / 1000;
    while (left > 0 && b.path.length) {
      const wp = b.path[0]!;
      const dx = wp.x - c.pos.x;
      const dy = wp.y - c.pos.y;
      const d = Math.hypot(dx, dy);
      if (d <= Math.max(left, 0.001)) {
        c.pos.x = wp.x;
        c.pos.y = wp.y;
        left -= d;
        b.path.shift();
        continue;
      }
      const nx = c.pos.x + (dx / d) * left;
      const ny = c.pos.y + (dy / d) * left;
      if (nav && !nav.walkable(nx, ny) && nav.walkable(c.pos.x, c.pos.y)) {
        // Something shoved us off the planned line (knockback): plan again next think.
        this.stop(b);
        b.nextThink = this.now;
        break;
      }
      c.pos.x = nx;
      c.pos.y = ny;
      if (b.moveAnim !== Anim.Strafe) c.yaw = Math.atan2(-dx, dy);
      left = 0;
    }
    if (nav && nav.walkable(c.pos.x, c.pos.y)) c.pos.z = nav.heightAt(c.pos.x, c.pos.y);
    if (c.anim !== Anim.Attack || b.path.length) c.anim = b.moveAnim;
    if (!b.path.length || dist(c.pos, b.goal) < ARRIVE) {
      b.path = [];
      b.goal = null;
    }
  }

  private between([lo, hi]: [number, number]): number {
    return lo + this.rng() * (hi - lo);
  }
}

const dist = (a: Point2, b: Point2) => Math.hypot(a.x - b.x, a.y - b.y);
const dist2 = (a: Point2, b: Point2) => (a.x - b.x) ** 2 + (a.y - b.y) ** 2;
/** Same yaw convention as the hero sim: forward is (-sin yaw, cos yaw). */
const yawTo = (from: Point2, to: Point2) => Math.atan2(-(to.x - from.x), to.y - from.y);
const wrap = (a: number) => Math.atan2(Math.sin(a), Math.cos(a));
const jitter = (p: Point2, r: number, rng: Rng): Point2 => {
  const ang = rng() * Math.PI * 2;
  const d = Math.sqrt(rng()) * r;
  return { x: p.x + Math.cos(ang) * d, y: p.y + Math.sin(ang) * d };
};
