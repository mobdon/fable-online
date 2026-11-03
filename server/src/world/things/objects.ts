import type { EventSink } from '../../analytics/events.js';
import type { Database } from '../../db/database.js';
import type { Economy } from '../../economy/economy.js';
import { GameError } from '../../errors.js';
import type { Channel, PlayerLink } from '../channel.js';
import type { Director } from '../director.js';
import type { Vec3 } from '../sim.js';
import { lootItemDef, resolveLoot } from './loot.js';
import type { AnyThing, DoorThing, ExitThing, Link, PickupThing, WorldThings } from './world-things.js';

/** How close a hero must stand (metres, across the ground) to use a thing. */
export const REACH = 3;
/** Doors are wide and swing; allow a little more. */
export const DOOR_REACH = 4;
/** Demon doors are talked to from a distance. */
export const DEMON_DOOR_REACH = 10;
/** And no more than this far above or below it. */
export const REACH_Z = 3;
/** A door someone opened swings shut again after this long. */
export const DOOR_CLOSE_MS = 20_000;
/** Common items lying in the world come back this long after someone takes them. */
export const PICKUP_RESPAWN_MS = 10 * 60_000;
/** Cupboards and bookshelves can be searched again by the same hero after this long. */
export const SEARCH_AGAIN_MS = 24 * 60 * 60_000;
/** A hero must step this much further than an exit's radius before it can fire again. */
export const EXIT_REARM = 1.25;

const SILVER_KEY = 'OBJECT_SILVER_KEY';
const SPADE = 'OBJECT_SPADE';

/**
 * A rule that can stop a hero using a thing (a locked prison door, a demon door that is
 * still shut, a jailed hero trying to leave). Returns a reason code, or null to allow.
 * The crime system adds its own gates here.
 */
export type Gate = (heroId: number, thing: AnyThing) => { code: string; message: string } | null;

interface ChannelState {
  /** door id -> open, only for doors not in their level-file state */
  doors: Map<string, { open: boolean; closeAt: number }>;
  /** shared pickup id -> when it comes back */
  taken: Map<string, number>;
}

interface HeroState {
  /** False right after arriving, until the hero has stepped clear of every exit. */
  armed: boolean;
}

interface PendingMove {
  heroId: number;
  link: Link;
  via: 'exit' | 'door';
  thing: string;
}

export type LootResult = {
  t: 'loot';
  thing: string;
  gold: number;
  items: { item_id: string; def: string }[];
  blocked: string[];
  /** true when this hero had already taken it: nothing new was created. */
  again: boolean;
};

/**
 * Server-side doors, chests, containers, digging spots, world pickups, demon doors and
 * region exits.
 *
 * Who may take what:
 *  - Chests and digging spots: every hero once, ever. The ledger claim key
 *    "chest:<hero>:<thing>" is the record, so it holds across channels, restarts and
 *    reconnects, and a doubled request is a replay that creates nothing.
 *  - Cupboards and bookshelves: every hero once a day.
 *  - Unique things lying in the world (silver keys, quest items, things the level marks
 *    persistent): every hero once, ever, each in their own copy of the world.
 *  - Common things lying in the world (potions, food): shared by the channel. The first
 *    hero to reach one gets it, it is gone for everyone in that channel, and it comes back
 *    after PICKUP_RESPAWN_MS. Each hero can take it once per respawn window.
 *  - Doors: shared by the channel, everyone sees the same door open or shut.
 *
 * Every use is checked against the hero's server position (REACH), never the client's.
 */
export class WorldObjects {
  readonly gates: Gate[] = [];
  private channels = new Map<string, ChannelState>();
  private heroes = new Map<number, HeroState>();
  private moves: PendingMove[] = [];

  constructor(
    readonly things: WorldThings,
    private director: Director,
    private economy: Economy,
    private db: Database,
    private events: EventSink,
  ) {
    director.hooks = {
      onEnter: (ch, link) => this.onEnter(ch, link),
      onTick: (ch, now) => this.onTick(ch, now),
      afterTick: () => this.flushMoves(),
      onLeave: (_ch, heroId) => this.heroes.delete(heroId),
      onClose: (ch) => this.channels.delete(ch.id),
    };
    // A demon door keeps the exit behind it shut until this hero has opened it.
    this.gates.push((heroId, t) => {
      if (t.kind !== 'exit') return null;
      const door = this.things.guardedExits.get(t.id);
      if (!door || this.hasFlag(heroId, `demon_door:${door}`)) return null;
      return { code: 'demon_door_closed', message: 'The demon door has not opened for you' };
    });
    // Prison cell doors are opened by the guards, not by the prisoner.
    this.gates.push((_heroId, t) => (t.kind === 'door' && t.prison ? { code: 'locked', message: 'It is locked' } : null));
  }

  // ---------------------------------------------------------------- player actions

  /** A hero used a thing. Returns the message to send back to that hero. */
  interact(heroId: number, thingId: string, action = 'use'): Record<string, unknown> {
    const ch = this.director.channelOf(heroId);
    const t = this.things.get(thingId);
    try {
      if (!ch) throw new GameError('offline', 'Hero is not in the world');
      if (!t || t.region !== ch.region.name) throw new GameError('not_here', 'That is not here');
      if (t.kind === 'exit') throw new GameError('walk_through', 'Walk through it');
      if (ch.loading.has(heroId)) throw new GameError('loading', 'Still arriving');
      const reach = t.kind === 'door' ? DOOR_REACH : t.kind === 'demon_door' ? DEMON_DOOR_REACH : REACH;
      this.mustReach(ch, heroId, t.pos, reach);
      this.mustPass(heroId, t);
      const out = this.use(ch, heroId, t, action);
      this.events.emit({ type: 'interact', heroId, thing: t.id, kind: t.kind, ok: true });
      return out;
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      this.events.emit({ type: 'interact', heroId, thing: thingId, kind: t?.kind ?? 'unknown', ok: false, reason: e.code });
      return { t: 'error', error: e.code, message: e.message, thing: thingId };
    }
  }

  private use(ch: Channel, heroId: number, t: AnyThing, action: string): Record<string, unknown> {
    switch (t.kind) {
      case 'door':
        return this.useDoor(ch, t, action);
      case 'chest':
        if (t.silverKeys > 0 && this.economy.countOf(heroId, SILVER_KEY) < t.silverKeys)
          throw new GameError('need_keys', `This chest needs ${t.silverKeys} silver keys`);
        return this.loot(heroId, t, `chest:${heroId}:${t.id}`, t.contents);
      case 'dig':
        if (this.economy.countOf(heroId, SPADE) < 1) throw new GameError('need_spade', 'You need a spade');
        return this.loot(heroId, t, `dig:${heroId}:${t.id}`, t.contents);
      case 'container': {
        const day = Math.floor(Date.now() / SEARCH_AGAIN_MS);
        if (!t.contents.length) return { t: 'loot', thing: t.id, gold: 0, items: [], blocked: [], again: false };
        return this.loot(heroId, t, `search:${heroId}:${t.id}:${day}`, t.contents);
      }
      case 'pickup':
        return this.pickup(ch, heroId, t);
      case 'demon_door':
        // The riddle itself runs in the hero's own game, like every quest. The server
        // records that it opened (in reach, once) and unlocks the exit behind it; the
        // reward chests behind it are ordinary once-per-hero chests.
        if (action !== 'opened') throw new GameError('bad_action', 'Speak to the door');
        this.setFlag(heroId, `demon_door:${t.id}`);
        return { t: 'object', id: t.id, opened: true };
      case 'hook':
        if (!t.to) throw new GameError('no_link', 'It does not lead anywhere');
        this.moves.push({ heroId, link: t.to, via: 'door', thing: t.id });
        this.flushMoves();
        return { t: 'ok', thing: t.id };
      case 'exit':
        throw new GameError('walk_through', 'Walk through it');
    }
  }

  private useDoor(ch: Channel, d: DoorThing, action: string): Record<string, unknown> {
    const st = this.state(ch);
    const now = this.doorOpen(st, d);
    const open = action === 'open' ? true : action === 'close' ? false : !now;
    if (open === d.open) st.doors.delete(d.id);
    else st.doors.set(d.id, { open, closeAt: Date.now() + DOOR_CLOSE_MS });
    const msg = { t: 'object', id: d.id, open };
    if (open !== now) ch.broadcast(JSON.stringify(msg));
    return msg;
  }

  private pickup(ch: Channel, heroId: number, p: { kind: 'pickup' } & PickupThing): Record<string, unknown> {
    if (p.forSale) throw new GameError('for_sale', 'Buy it from the shopkeeper');
    if (isUnique(p)) {
      const r = this.loot(heroId, p, `pickup:${heroId}:${p.id}`, [p.def]);
      if (r.again) throw new GameError('gone', 'You already took that');
      this.stolen(heroId, p);
      return r;
    }
    const st = this.state(ch);
    const now = Date.now();
    const back = st.taken.get(p.id);
    if (back !== undefined && back > now) throw new GameError('gone', 'Someone took it');
    const window = Math.floor(now / PICKUP_RESPAWN_MS);
    const r = this.loot(heroId, p, `pickup:${heroId}:${p.id}:${window}`, [p.def]);
    if (r.again) throw new GameError('gone', 'You already took that');
    st.taken.set(p.id, now + PICKUP_RESPAWN_MS);
    ch.broadcast(JSON.stringify({ t: 'object', id: p.id, gone: true }));
    this.stolen(heroId, p);
    return r;
  }

  private stolen(heroId: number, p: { kind: 'pickup' } & PickupThing): void {
    // Stealing is the crime system's business; it hears about it here.
    if (p.stealable) this.events.emit({ type: 'theft', heroId, thing: p.id, def: p.def, region: p.region });
  }

  /** Resolve the contents and hand them over in one ledger transaction keyed by `key`. */
  private loot(heroId: number, t: AnyThing, key: string, contents: string[]): LootResult {
    const entries = resolveLoot(contents, key);
    let gold = 0;
    const items: { defId: string; quantity: number }[] = [];
    const blocked: string[] = [];
    for (const e of entries) {
      if ('gold' in e) gold += e.gold;
      else if ('blocked' in e) blocked.push(e.blocked);
      else {
        if (!this.economy.catalog.has(e.item)) this.economy.catalog.add(lootItemDef(e.item));
        items.push({ defId: e.item, quantity: e.quantity });
      }
    }
    const r = this.economy.claimLoot(heroId, key, gold, items, t.id);
    if (!r.replayed) this.events.emit({ type: 'loot', heroId, thing: t.id, txId: r.txId, gold, items: items.length, blocked });
    return {
      t: 'loot',
      thing: t.id,
      gold: r.replayed ? 0 : gold,
      items: r.replayed ? [] : r.itemIds.map((id, i) => ({ item_id: id, def: items[i]!.defId })),
      blocked,
      again: r.replayed,
    };
  }

  // ---------------------------------------------------------------- director hooks

  /** Tell a hero arriving in a channel how its things stand right now. */
  private onEnter(ch: Channel, link: PlayerLink): void {
    this.heroes.set(link.heroId, { armed: false });
    const st = this.state(ch);
    const now = Date.now();
    const doors: [string, boolean][] = [...st.doors].map(([id, d]) => [id, d.open]);
    const gone = [...st.taken].filter(([, at]) => at > now).map(([id]) => id);
    const opened: string[] = [];
    const region = ch.region.name;
    for (const prefix of ['chest', 'dig', 'pickup']) {
      for (const { k } of this.db.all<{ k: string }>('SELECT idempotency_key AS k FROM ledger_tx WHERE idempotency_key LIKE ?', `${prefix}:${link.heroId}:%`)) {
        const id = k.split(':').slice(2, 4).join(':');
        const t = this.things.get(id);
        if (t?.region !== region) continue;
        if (t.kind === 'pickup') {
          if (isUnique(t)) gone.push(id);
        } else opened.push(id);
      }
    }
    for (const { flag } of this.db.all<{ flag: string }>("SELECT flag FROM hero_flags WHERE hero_id = ? AND flag LIKE 'demon_door:%'", link.heroId)) {
      const id = flag.slice('demon_door:'.length);
      if (this.things.get(id)?.region === region) opened.push(id);
    }
    link.send(JSON.stringify({ t: 'objects', region, doors, gone, opened }));
  }

  private onTick(ch: Channel, now: number): void {
    const st = this.channels.get(ch.id);
    if (st) {
      for (const [id, d] of st.doors) {
        if (d.closeAt > now) continue;
        st.doors.delete(id);
        const def = this.things.get(id) as DoorThing | undefined;
        if (def && def.open !== d.open) ch.broadcast(JSON.stringify({ t: 'object', id, open: def.open }));
      }
      for (const [id, at] of st.taken) {
        if (at > now) continue;
        st.taken.delete(id);
        ch.broadcast(JSON.stringify({ t: 'object', id, gone: false }));
      }
    }
    const exits = this.things.region(ch.region.name).exits;
    if (!exits.length) return;
    for (const heroId of ch.players.keys()) {
      if (ch.loading.has(heroId)) continue;
      const hs = this.heroes.get(heroId);
      const p = ch.sim.position(heroId);
      if (!hs || !p) continue;
      let inside: ExitThing | undefined;
      let near = false;
      for (const e of exits) {
        if (!usableExit(e)) continue;
        const d = Math.hypot(p.x - e.pos[0], p.y - e.pos[1]);
        if (Math.abs(p.z - e.pos[2]) > REACH_Z + e.radius) continue;
        if (d <= e.radius) inside = e;
        if (d <= e.radius * EXIT_REARM) near = true;
      }
      if (!near) hs.armed = true;
      if (!inside || !hs.armed) continue;
      hs.armed = false; // one try per approach, so a shut exit doesn't spam
      const no = this.gate(heroId, { kind: 'exit', ...inside });
      if (no) {
        ch.players.get(heroId)?.send(JSON.stringify({ t: 'error', error: no.code, message: no.message, thing: inside.id }));
        continue;
      }
      this.moves.push({ heroId, link: inside.to!, via: 'exit', thing: inside.id });
    }
  }

  private flushMoves(): void {
    const moves = this.moves;
    this.moves = [];
    for (const m of moves) {
      const from = this.director.channelOf(m.heroId);
      if (!from) continue;
      try {
        const yaw = m.link.fwd ? Math.atan2(-m.link.fwd[0], m.link.fwd[1]) : undefined;
        const at: Vec3 = { x: m.link.pos[0], y: m.link.pos[1], z: m.link.pos[2] };
        // A door's link comes straight from the level data, so it is trusted like the
        // graph. Exits are still checked against the region graph by the director.
        this.director.transfer(m.heroId, m.link.region, at, m.via, { entrance: m.link.entrance, yaw, thing: m.thing });
        this.events.emit({ type: 'region_exit', heroId: m.heroId, from: from.region.name, to: m.link.region, via: m.via, thing: m.thing });
      } catch (e) {
        if (!(e instanceof GameError)) throw e;
        from.players.get(m.heroId)?.send(JSON.stringify({ t: 'error', error: e.code, message: e.message, thing: m.thing }));
      }
    }
  }

  // ---------------------------------------------------------------- helpers

  private mustReach(ch: Channel, heroId: number, pos: [number, number, number], reach: number): void {
    const p = ch.sim.position(heroId);
    if (!p || Math.hypot(p.x - pos[0], p.y - pos[1]) > reach || Math.abs(p.z - pos[2]) > REACH_Z)
      throw new GameError('too_far', 'Too far away');
  }

  private mustPass(heroId: number, t: AnyThing): void {
    const no = this.gate(heroId, t);
    if (no) throw new GameError(no.code, no.message, 403);
  }

  private gate(heroId: number, t: AnyThing): { code: string; message: string } | null {
    for (const g of this.gates) {
      const no = g(heroId, t);
      if (no) return no;
    }
    return null;
  }

  private state(ch: Channel): ChannelState {
    let st = this.channels.get(ch.id);
    if (!st) this.channels.set(ch.id, (st = { doors: new Map(), taken: new Map() }));
    return st;
  }

  private doorOpen(st: ChannelState, d: DoorThing): boolean {
    return st.doors.get(d.id)?.open ?? d.open;
  }

  hasFlag(heroId: number, flag: string): boolean {
    return this.db.get('SELECT 1 FROM hero_flags WHERE hero_id = ? AND flag = ?', heroId, flag) !== undefined;
  }

  setFlag(heroId: number, flag: string): void {
    this.db.run('INSERT OR IGNORE INTO hero_flags (hero_id, flag, set_at) VALUES (?, ?, ?)', heroId, flag, Date.now());
  }
}

/** Exits that always exist and lead somewhere. Quest-section exits belong to the client's quest. */
function usableExit(e: ExitThing): boolean {
  return e.active && e.to !== null && e.section === 'NULL';
}

/** Items that exist once per hero rather than being shared and respawning. */
function isUnique(p: PickupThing): boolean {
  return p.persistent || p.section !== 'NULL' || /KEY|TROPHY|CARD|BOOK/.test(p.def);
}
