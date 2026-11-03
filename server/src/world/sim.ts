import type { RegionDef } from './regions.js';

export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

/** What a client is allowed to send each frame: intent only, never a position. */
export interface PlayerInput {
  seq: number;
  moveX: number; // -1..1 strafe
  moveY: number; // -1..1 forward
  buttons: number;
  cameraYaw: number; // radians
}

export interface EntityState {
  id: number;
  def_id: number;
  x: number;
  y: number;
  z: number;
  yaw: number;
  anim: number;
  health: number;
  /** Creatures only: Fable def name, so the client knows which local creature to drive. */
  def?: string;
}

/**
 * The simulation behind one channel. In production this is a server-mode Fable.exe that
 * the region host drives through the mod (phase 4); the server feeds it inputs and reads
 * entity state back. Until the memory map from phase 2 is done, StubSim stands in so the
 * whole pipeline (accounts -> join -> channel -> ticks -> persistence) can be built and
 * load-tested now.
 */
export interface RegionSim {
  addHero(heroId: number, at: Vec3): void;
  removeHero(heroId: number): Vec3 | undefined;
  applyInput(heroId: number, input: PlayerInput): void;
  step(dtMs: number): void;
  snapshot(): EntityState[];
  position(heroId: number): Vec3 | undefined;
  dispose(): void;
}

export type SimFactory = (region: RegionDef, channelId: string) => RegionSim;

/** Kinematic stand-in: heroes walk at Fable's run speed in the direction of their input. */
export class StubSim implements RegionSim {
  static readonly RUN_SPEED = 6; // metres per second
  static readonly BOUNDS = 512;
  private heroes = new Map<number, { pos: Vec3; yaw: number; move: [number, number]; anim: number; health: number }>();

  addHero(heroId: number, at: Vec3): void {
    this.heroes.set(heroId, { pos: { ...at }, yaw: 0, move: [0, 0], anim: 0, health: 100 });
  }
  removeHero(heroId: number): Vec3 | undefined {
    const h = this.heroes.get(heroId);
    this.heroes.delete(heroId);
    return h?.pos;
  }
  applyInput(heroId: number, input: PlayerInput): void {
    const h = this.heroes.get(heroId);
    if (!h) return;
    h.yaw = input.cameraYaw;
    h.move = [input.moveX, input.moveY];
  }
  step(dtMs: number): void {
    const d = (StubSim.RUN_SPEED * dtMs) / 1000;
    const b = StubSim.BOUNDS;
    for (const h of this.heroes.values()) {
      const [mx, my] = h.move;
      if (mx === 0 && my === 0) {
        h.anim = 0;
        continue;
      }
      const s = Math.sin(h.yaw);
      const c = Math.cos(h.yaw);
      h.pos.x = Math.max(-b, Math.min(b, h.pos.x + (mx * c - my * s) * d));
      h.pos.y = Math.max(-b, Math.min(b, h.pos.y + (mx * s + my * c) * d));
      h.anim = 1;
    }
  }
  snapshot(): EntityState[] {
    const out: EntityState[] = [];
    for (const [id, h] of this.heroes)
      out.push({ id, def_id: 0, x: round(h.pos.x), y: round(h.pos.y), z: round(h.pos.z), yaw: round(h.yaw), anim: h.anim, health: h.health });
    return out;
  }
  position(heroId: number): Vec3 | undefined {
    const p = this.heroes.get(heroId)?.pos;
    return p && { ...p };
  }
  dispose(): void {
    this.heroes.clear();
  }
}

const round = (n: number) => Math.round(n * 100) / 100;

export const stubSimFactory: SimFactory = () => new StubSim();
