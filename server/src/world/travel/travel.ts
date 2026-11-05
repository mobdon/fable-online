import type { EventSink } from '../../analytics/events.js';
import type { Database } from '../../db/database.js';
import { GameError } from '../../errors.js';
import type { Heroes } from '../../heroes/heroes.js';
import type { Channel } from '../channel.js';
import type { Director } from '../director.js';
import { STAGE_REGIONS } from '../regions.js';
import type { Vec3 } from '../sim.js';
import type { GateThing, WorldThings } from '../things/world-things.js';

/**
 * Server rules for Cullis Gate and Guild Seal travel.
 *
 * Fable's own rule: once a hero has been to a Cullis Gate, they can teleport to it at any
 * time, either by walking into a gate or by charging the Guild Seal from anywhere that is
 * not barred to teleporting, and after a seal jump they can go back to where they left.
 *
 * Here the server owns all of it. The client asks ("take me to gate X", "take me back");
 * the server checks and, if allowed, moves the hero with Director.transfer('cullis_gate'),
 * which sends the usual enter_region with the gate as the arrival thing. Nothing the client
 * says unlocks a gate: a gate unlocks when the server's own copy of the hero stands near it.
 */
export interface TravelRules {
  /** Metres from a gate at which a hero unlocks it by being there. */
  unlockRadius: number;
  /** Metres from a gate at which a hero counts as standing in it (gate-to-gate travel, no seal). */
  reach: number;
  /** No travel this long after dealing or taking a hit. */
  combatLockMs: number;
  /** Minimum time between two jumps by the same hero. */
  cooldownMs: number;
  /** Party members this close to the hero who travels go along. */
  partyRadius: number;
  /** Arrivals step this far out of the gate, along its facing. */
  arrivalStep: number;
  /** Regions no one can teleport out of or back into (Fable's teleport barriers, prisons). */
  barred: RegExp[];
  /** Every gate counts as found (test servers, FO_GATES_OPEN). */
  allOpen?: boolean;
}

export const TRAVEL_RULES: TravelRules = {
  unlockRadius: 12,
  reach: 4,
  combatLockMs: 10_000,
  cooldownMs: 10_000,
  partyRadius: 20,
  arrivalStep: 2.5,
  barred: [/^DemonDoor_/, /^Prison/, /^BowerstoneJail$/, /^Arena$/, /^ArenaCells$/],
};

/**
 * Parties are not built yet. When they are, they register this and a hero's nearby party
 * members travel with them.
 */
export interface PartyLookup {
  /** The other members of this hero's party, or an empty list. */
  members(heroId: number): number[];
}

/** Where a hero can go, as sent to the client. */
export interface GateEntry {
  id: string;
  region: string;
  guild: boolean;
}

export type TravelTarget = { gate: string } | { guild: true } | { back: true };

interface ReturnPoint {
  region: string;
  pos: Vec3;
}

/** Throttle for the unlock scan; nobody walks past a gate in under a second. */
const SCAN_MS = 1000;
const FLAG = 'gate:';

export class Travel {
  parties: PartyLookup | null = null;
  private byRegion = new Map<string, GateThing[]>();
  private guild: GateThing | undefined;
  /** Online heroes' unlocked gates, read from hero_flags on first use. */
  private unlocked = new Map<number, Set<string>>();
  private lastJump = new Map<number, number>();
  private returns = new Map<number, ReturnPoint>();
  private lastScan = 0;
  private readonly gates: ReadonlyMap<string, GateThing>;

  constructor(
    things: WorldThings,
    private director: Director,
    private heroes: Heroes,
    private db: Database,
    private events: EventSink,
    readonly rules: TravelRules = { ...TRAVEL_RULES, barred: [...TRAVEL_RULES.barred] },
  ) {
    for (const g of things.gates.values()) {
      const list = this.byRegion.get(g.region) ?? [];
      list.push(g);
      this.byRegion.set(g.region, list);
      if (g.guild && !this.guild) this.guild = g;
    }
    this.gates = things.gates;
    director.policies.push({
      tick: (_dt, ids) => this.scan(ids),
      left: (heroId) => {
        this.unlocked.delete(heroId);
        this.lastJump.delete(heroId);
      },
    });
  }

  isGate(id: string): boolean {
    return this.gates.has(id);
  }

  // ---------------------------------------------------------------- client requests

  /** Every gate this hero can travel to, plus the Guild and the way back after a seal jump. */
  list(heroId: number): Record<string, unknown> {
    const open = this.unlockedOf(heroId);
    const gates: GateEntry[] = [];
    for (const g of this.gates.values()) if (g.guild || this.rules.allOpen || open.has(g.id)) gates.push({ id: g.id, region: g.region, guild: g.guild });
    const back = this.returns.get(heroId);
    return { t: 'gates', gates, guild: true, back: back ? { region: back.region } : null };
  }

  /**
   * The client's Fable says the hero walked into a gate (the gate's own travel menu). The
   * server unlocks it if its copy of the hero is there, and answers with the list.
   */
  useGate(heroId: number, gateId: string): Record<string, unknown> {
    const ch = this.director.channelOf(heroId);
    const g = this.gates.get(gateId);
    if (!ch || !g || g.region !== ch.region.name) return { t: 'error', error: 'not_here', message: 'That is not here', thing: gateId };
    const pos = ch.sim.position(heroId);
    if (!pos || dist(pos, g.pos) > this.rules.reach + 1) return { t: 'error', error: 'too_far', message: 'You are not at that gate', thing: gateId };
    this.unlock(heroId, g);
    return this.list(heroId);
  }

  /**
   * Travel. `from` names the gate the hero stands in (gate-to-gate); without it this is a
   * Guild Seal jump from wherever the hero is, which also remembers the way back.
   * Party members within partyRadius who may travel go along to the same place.
   */
  travel(heroId: number, target: TravelTarget, from?: string): Record<string, unknown> {
    const label = 'gate' in target ? target.gate : 'guild' in target ? 'guild' : 'back';
    try {
      const ch = this.director.channelOf(heroId);
      if (!ch) throw new GameError('offline', 'Not in the world');
      const pos = ch.sim.position(heroId);
      if (!pos) throw new GameError('offline', 'Not in the world');
      const now = Date.now();
      this.mayTravel(heroId, ch, now);
      if (from !== undefined) {
        const g = this.gates.get(from);
        if (!g || g.region !== ch.region.name || dist(pos, g.pos) > this.rules.reach + 1) throw new GameError('too_far', 'You are not at that gate');
        this.unlock(heroId, g);
      }
      const dest = this.destination(heroId, target);
      if (dest.region === ch.region.name && dist(pos, dest.pos) <= this.rules.unlockRadius) throw new GameError('already_here', 'You are already there');

      // Who goes: the hero, and party members nearby who may travel too.
      const goers = [heroId];
      const behind: { heroId: number; error: string }[] = [];
      for (const m of this.parties?.members(heroId) ?? []) {
        if (m === heroId) continue;
        const mp = this.director.channelOf(m) === ch ? ch.sim.position(m) : undefined;
        if (!mp || dist(mp, pos) > this.rules.partyRadius) continue;
        try {
          this.mayTravel(m, ch, now);
          goers.push(m);
        } catch (e) {
          if (!(e instanceof GameError)) throw e;
          behind.push({ heroId: m, error: e.code });
        }
      }

      const seal = from === undefined && !('back' in target);
      const moved: number[] = [];
      goers.forEach((id, i) => {
        const here = id === heroId ? pos : ch.sim.position(id)!;
        // Party members fan out around the arrival point instead of landing in one spot.
        const at = i === 0 ? dest.pos : around(dest.pos, i);
        try {
          this.director.transfer(id, dest.region, at, 'cullis_gate', { thing: dest.gate ?? undefined });
        } catch (e) {
          if (id === heroId || !(e instanceof GameError)) throw e;
          behind.push({ heroId: id, error: e.code });
          return;
        }
        moved.push(id);
        this.lastJump.set(id, now);
        if (seal) this.returns.set(id, { region: ch.region.name, pos: here });
        else if ('back' in target) this.returns.delete(id);
        this.events.emit({ type: 'gate_travel', heroId: id, from: ch.region.name, to: dest.region, gate: dest.gate, via: seal ? 'seal' : 'back' in target ? 'back' : 'gate', leader: id === heroId ? undefined : heroId });
        if (id !== heroId) this.director.sendTo(id, JSON.stringify({ t: 'travel_result', ok: true, to: label, region: dest.region, with: heroId }));
      });
      for (const b of behind) this.director.sendTo(b.heroId, JSON.stringify({ t: 'travel_result', ok: false, to: label, error: b.error, left_behind_by: heroId }));
      return { t: 'travel_result', ok: true, to: label, region: dest.region, party: moved.filter((id) => id !== heroId) };
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      this.events.emit({ type: 'gate_travel', heroId, to: label, refused: e.code });
      return { t: 'travel_result', ok: false, to: label, error: e.code, message: e.message };
    }
  }

  // ---------------------------------------------------------------- rules

  /** Throws the reason this hero may not jump right now. */
  private mayTravel(heroId: number, ch: Channel, now: number): void {
    const hero = this.heroes.get(heroId);
    if (!hero || hero.phase !== 'world') throw new GameError('not_graduated', 'Finish your Guild training first');
    if (ch.kind === 'private') throw new GameError('barred', 'You cannot teleport from here');
    if (ch.loading.has(heroId)) throw new GameError('loading', 'Still arriving');
    if (this.barred(ch.region.name)) throw new GameError('barred', 'A magical barrier stops you teleporting');
    const c = ch.sim.combatState?.(heroId);
    if (c && !c.alive) throw new GameError('dead', 'You are down');
    if (c && c.lastHitAt && now - c.lastHitAt < this.rules.combatLockMs) throw new GameError('in_combat', 'You cannot teleport in the middle of a fight');
    const last = this.lastJump.get(heroId);
    if (last !== undefined && now - last < this.rules.cooldownMs) throw new GameError('cooldown', 'Your Guild Seal is still recharging');
  }

  private destination(heroId: number, target: TravelTarget): { region: string; pos: Vec3; gate: string | null } {
    if ('back' in target) {
      const r = this.returns.get(heroId);
      if (!r) throw new GameError('no_return', 'There is nowhere to go back to');
      if (this.barred(r.region)) throw new GameError('barred', 'A magical barrier stops you teleporting there');
      return { region: r.region, pos: r.pos, gate: null };
    }
    if ('guild' in target) {
      // Without extracted gates the hero still reaches the Guild, wherever its Fable puts them.
      if (!this.guild) return { region: STAGE_REGIONS.world, pos: { x: 0, y: 0, z: 0 }, gate: null };
      return { region: this.guild.region, pos: this.arrival(this.guild), gate: this.guild.id };
    }
    const g = this.gates.get(target.gate);
    if (!g) throw new GameError('no_gate', 'No such gate');
    if (!g.guild && !this.rules.allOpen && !this.unlockedOf(heroId).has(g.id)) throw new GameError('locked', 'You have not found that Cullis Gate yet');
    if (this.barred(g.region)) throw new GameError('barred', 'A magical barrier stops you teleporting there');
    return { region: g.region, pos: this.arrival(g), gate: g.id };
  }

  private barred(region: string): boolean {
    return this.rules.barred.some((r) => r.test(region));
  }

  /** Just in front of the gate, so the hero does not land inside it. */
  private arrival(g: GateThing): Vec3 {
    const [x, y, z] = g.pos;
    if (!g.fwd) return { x, y, z };
    const len = Math.hypot(g.fwd[0], g.fwd[1]) || 1;
    const s = this.rules.arrivalStep / len;
    return { x: x + g.fwd[0] * s, y: y + g.fwd[1] * s, z };
  }

  // ---------------------------------------------------------------- unlocks

  unlockedOf(heroId: number): Set<string> {
    let s = this.unlocked.get(heroId);
    if (!s) {
      s = new Set(
        this.db
          .all<{ flag: string }>('SELECT flag FROM hero_flags WHERE hero_id = ? AND flag LIKE ?', heroId, `${FLAG}%`)
          .map((r) => r.flag.slice(FLAG.length)),
      );
      // Only keep the cache for heroes in the world; `left` clears it.
      if (this.director.channelOf(heroId)) this.unlocked.set(heroId, s);
    }
    return s;
  }

  private unlock(heroId: number, g: GateThing): void {
    if (g.guild) return;
    const s = this.unlockedOf(heroId);
    if (s.has(g.id)) return;
    if (this.heroes.get(heroId)?.phase !== 'world') return;
    s.add(g.id);
    this.db.run('INSERT OR IGNORE INTO hero_flags (hero_id, flag, set_at) VALUES (?, ?, ?)', heroId, FLAG + g.id, Date.now());
    this.events.emit({ type: 'gate_unlocked', heroId, gate: g.id, region: g.region });
    this.director.sendTo(heroId, JSON.stringify({ t: 'gate_unlocked', id: g.id, region: g.region }));
  }

  /** About once a second: unlock gates that heroes in the world are standing near. */
  private scan(ids: Iterable<number>): void {
    const now = Date.now();
    if (now - this.lastScan < SCAN_MS || !this.byRegion.size) return;
    this.lastScan = now;
    const r = this.rules.unlockRadius;
    for (const id of ids) {
      const ch = this.director.channelOf(id);
      if (!ch || ch.kind === 'private' || ch.loading.has(id)) continue;
      const gates = this.byRegion.get(ch.region.name);
      if (!gates) continue;
      const pos = ch.sim.position(id);
      if (!pos) continue;
      for (const g of gates) if (!g.guild && dist(pos, g.pos) <= r) this.unlock(id, g);
    }
  }
}

const dist = (a: Vec3, b: [number, number, number] | Vec3) => {
  const [bx, by] = Array.isArray(b) ? b : [b.x, b.y];
  return Math.hypot(a.x - bx, a.y - by);
};

/** Spots on a small ring around `p` for the i-th party member. */
function around(p: Vec3, i: number): Vec3 {
  const a = (i * Math.PI * 2) / 5;
  return { x: p.x + Math.cos(a) * 1.5, y: p.y + Math.sin(a) * 1.5, z: p.z };
}
