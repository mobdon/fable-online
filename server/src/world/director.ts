import type { ServerConfig } from '../config.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';
import type { Hero } from '../heroes/heroes.js';
import { Channel, type PlayerLink } from './channel.js';
import { STAGE_REGIONS, type RegionTable } from './regions.js';
import type { PlayerInput, SimFactory, Vec3 } from './sim.js';
import type { WorldClock } from './clock.js';

const SKY_CHECK_MS = 1000;
const SKY_RESYNC_MS = 60_000;

export type PersistFn = (heroId: number, region: string, pos: Vec3) => void;

/** `client`: the client's own Fable already loaded the region (see Director.follow). */
export type TransferVia = 'exit' | 'door' | 'cullis_gate' | 'law' | 'client';

/** Extra detail for a region change, passed through to the client's enter_region. */
export interface ArrivalInfo {
  /** Thing id of the region entrance the hero arrives at, so the client loads to the same spot. */
  entrance?: string;
  yaw?: number;
  /** Thing id of the exit or door used. */
  thing?: string;
}

/** Other world systems that follow heroes and channels (doors, loot, exits). */
export interface DirectorHooks {
  /** A hero was placed in a channel (join or region change). */
  onEnter?(ch: Channel, link: PlayerLink): void;
  /** Called for each channel after its sim stepped. Must not move heroes between channels. */
  onTick?(ch: Channel, now: number): void;
  /** Called once per server frame after every channel stepped; may move heroes. */
  afterTick?(): void;
  onLeave?(ch: Channel, heroId: number): void;
  onClose?(ch: Channel): void;
}

/** How long a hero may take to load the next region before the server unfreezes them anyway. */
export const REGION_LOAD_TIMEOUT_MS = 30_000;

/**
 * Rules other systems put on placement. The law module uses them to keep prisoners in
 * their cells, keep heroes thrown out of a town out of it, and count online time.
 */
export interface DirectorPolicy {
  /** Where this hero must appear on login instead of their saved region. */
  placement?(hero: Hero): { region: string; private: boolean } | undefined;
  /** Refuse a region change by returning an error. 'law' moves are the law's own. */
  canEnter?(heroId: number, from: string, to: string, via: TransferVia): GameError | null;
  /** After every frame, with every hero in the world. */
  tick?(dtMs: number, heroIds: Iterable<number>): void;
  left?(heroId: number): void;
}

export interface DirectorStats {
  players: number;
  sharedChannels: number;
  privateInstances: number;
  regions: { region: string; channels: { id: string; players: number }[] }[];
  lastTickMs: number;
}

/**
 * Places players into region channels and scales channels up and down.
 *
 * Placement: a graduated hero goes to the fullest shared channel of their region that is
 * still under the soft cap (keeps the world feeling busy). If every channel is at the
 * soft cap, a new channel opens, up to the global maxChannels budget; past that, players
 * overflow into channels up to the hard cap, and only then is the region "full".
 * A hero still in childhood or Guild training always gets a private instance.
 * Empty channels close after channelIdleMs, freeing their Fable.exe slot.
 */
export class Director {
  private channels = new Map<string, Channel>();
  private shared = new Map<string, Channel[]>(); // region -> shared channels
  private where = new Map<number, Channel>(); // heroId -> channel
  private nextIndex = new Map<string, number>();
  private privateCount = 0;
  private lastPersist = Date.now();
  private lastSky = 0;
  private timer: NodeJS.Timeout | null = null;
  lastTickMs = 0;
  hooks: DirectorHooks = {};
  readonly policies: DirectorPolicy[] = [];

  constructor(
    private cfg: ServerConfig,
    private regions: RegionTable,
    private simFactory: SimFactory,
    private events: EventSink,
    private persist: PersistFn,
    private clock?: WorldClock,
  ) {}

  start(): void {
    const dt = 1000 / this.cfg.tickRate;
    let last = performance.now();
    let statsAt = Date.now();
    this.timer = setInterval(() => {
      const now = performance.now();
      this.tick(now - last);
      last = now;
      if (Date.now() - statsAt > 60_000) {
        statsAt = Date.now();
        this.events.emit({ type: 'tick_stats', channels: this.channels.size, players: this.where.size, tickMs: this.lastTickMs });
      }
    }, dt);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    for (const heroId of [...this.where.keys()]) this.leave(heroId);
    for (const c of [...this.channels.values()]) this.closeChannel(c);
  }

  /** Put a hero into the world. Region and position come from the database, never the client. */
  /** `resumed`: the same game session reconnecting, so its Fable still has the hero where the server does. */
  join(hero: Hero, link: PlayerLink, resumed = false): Channel {
    if (this.where.has(hero.id)) throw new GameError('already_online', 'That hero is already in the world', 409);
    let region: string = hero.phase === 'world' ? hero.region : STAGE_REGIONS[hero.phase];
    let solo = hero.phase !== 'world';
    for (const p of this.policies) {
      const forced = hero.phase === 'world' ? p.placement?.(hero) : undefined;
      if (forced && this.regions.has(forced.region)) {
        region = forced.region;
        solo = forced.private;
        break;
      }
    }
    const def = this.regions.get(region) ?? this.regions.get(STAGE_REGIONS.world);
    if (!def) throw new GameError('no_region', `Unknown region ${region}`, 500);
    const ch = solo ? this.openPrivate(def.name, hero.id) : this.pickShared(def.name);
    const at = { x: hero.x, y: hero.y, z: hero.z };
    // The client's Fable loads its own save, which need not be where the server last had the
    // hero, so the first position report may re-anchor them (see ServerConfig.joinAnchor).
    this.place(ch, link, at, undefined, isOrigin(at) || (this.cfg.joinAnchor && !resumed) ? Infinity : 0);
    return ch;
  }

  leave(heroId: number): void {
    const ch = this.where.get(heroId);
    if (!ch) return;
    const p = ch.players.get(heroId);
    const pos = ch.remove(heroId);
    this.where.delete(heroId);
    this.hooks.onLeave?.(ch, heroId);
    if (pos) this.persist(heroId, ch.region.name, pos);
    if (p) this.events.emit({ type: 'player_disconnected', heroId, channelId: ch.id, sessionMs: Date.now() - p.joinedAt });
    if (ch.kind === 'private') this.closeChannel(ch);
    for (const pol of this.policies) pol.left?.(heroId);
  }

  /**
   * Move a hero to another region. Called by the server when the sim reports the hero
   * walked through a region exit, used a door that leads elsewhere, or used a Cullis Gate.
   * Never called on client say-so.
   *
   * 'exit' must follow the region graph. 'door' and 'cullis_gate' are checked by the
   * caller against the level data (a door's link, a gate's network).
   *
   * The hero is frozen in the new channel until the client reports the region loaded.
   */
  transfer(heroId: number, toRegion: string, at: Vec3, via: TransferVia, info: ArrivalInfo = {}, solo?: boolean): Channel {
    const from = this.where.get(heroId);
    if (!from) throw new GameError('offline', 'Hero is not in the world');
    if (!this.regions.has(toRegion)) throw new GameError('no_region', `Unknown region ${toRegion}`);
    if (via === 'exit' && !this.regions.areNeighbours(from.region.name, toRegion))
      throw new GameError('not_adjacent', `${from.region.name} does not lead to ${toRegion}`);
    for (const p of this.policies) {
      const err = p.canEnter?.(heroId, from.region.name, toRegion, via);
      if (err) throw err;
    }
    const link = from.players.get(heroId)!;
    // Pick (or open) the target before leaving, so a full region leaves the hero where they are.
    const priv = solo ?? (via === 'law' ? false : from.kind === 'private');
    const target = priv ? this.openPrivate(toRegion, heroId) : this.pickShared(toRegion);
    from.remove(heroId);
    this.where.delete(heroId);
    this.hooks.onLeave?.(from, heroId);
    if (from.kind === 'private') this.closeChannel(from);
    this.persist(heroId, toRegion, at);
    // A client that loaded the region itself is already there: no wait, and the next report
    // comes from where it just was.
    if (via !== 'client') target.loading.set(heroId, Date.now() + REGION_LOAD_TIMEOUT_MS);
    // An arrival at the origin means "anywhere" (the law's moves); otherwise allow the
    // difference between the server's entrance and where the client's Fable puts the hero.
    this.place(target, link, at, { ...info, via, from: from.region.name }, isOrigin(at) ? Infinity : target.moveRules.arrivalSlack);
    return target;
  }

  /**
   * The client's Fable loaded another region by itself (the hero walked out of town) and the
   * server did not send it there. If the reported position lies in a region the server runs,
   * move the hero's server side to that region's shared channel, with no load on the client.
   * Returns the new channel, or undefined when nothing changed. Until the client's exits are
   * hooked this is how the server keeps up; the move guard has already accepted the position.
   */
  follow(heroId: number, at: Vec3): Channel | undefined {
    const ch = this.where.get(heroId);
    if (!ch || ch.kind === 'private' || !this.regions.canLocate) return undefined;
    const region = this.regions.locate(at.x, at.y);
    if (!region || region === this.regions.canonical(ch.region.name)) return undefined;
    const from = ch.region.name;
    try {
      const to = this.transfer(heroId, region, at, 'client');
      this.events.emit({ type: 'region_follow', heroId, from, to: region, channelId: to.id });
      return to;
    } catch (e) {
      this.events.emit({ type: 'region_follow', heroId, from, to: region, refused: (e as Error).message });
      return undefined;
    }
  }

  /** The client finished loading the region it was sent to. */
  regionReady(heroId: number, region: string): void {
    const ch = this.where.get(heroId);
    if (ch && ch.region.name === region) ch.loading.delete(heroId);
  }

  input(heroId: number, input: PlayerInput): void {
    this.where.get(heroId)?.input(heroId, input);
  }

  /** Send one frame to one online hero. */
  sendTo(heroId: number, frame: string): void {
    this.where.get(heroId)?.players.get(heroId)?.send(frame);
  }

  /** Send one frame to everyone in a channel. */
  broadcastTo(channelId: string, frame: string): void {
    this.channels.get(channelId)?.broadcast(frame);
  }

  channelOf(heroId: number): Channel | undefined {
    return this.where.get(heroId);
  }

  get online(): number {
    return this.where.size;
  }

  /** One server frame: step every channel, persist positions, close idle channels. */
  tick(dtMs: number): void {
    const t0 = performance.now();
    const now = Date.now();
    for (const c of this.channels.values()) {
      c.step(dtMs);
      this.hooks.onTick?.(c, now);
    }
    this.hooks.afterTick?.();
    for (const p of this.policies) p.tick?.(dtMs, this.where.keys());
    if (this.clock && now - this.lastSky >= SKY_CHECK_MS) {
      this.lastSky = now;
      for (const c of this.channels.values()) this.syncSky(c, now);
    }
    if (now - this.lastPersist >= this.cfg.persistEveryMs) {
      this.lastPersist = now;
      this.persistAll();
    }
    for (const c of [...this.channels.values()])
      if (c.kind === 'shared' && c.emptySince !== null && now - c.emptySince >= this.cfg.channelIdleMs) this.closeChannel(c);
    this.lastTickMs = performance.now() - t0;
  }

  persistAll(): void {
    for (const [heroId, ch] of this.where) {
      const pos = ch.sim.position(heroId);
      if (pos) this.persist(heroId, ch.region.name, pos);
    }
  }

  stats(): DirectorStats {
    const regions = [...this.shared.entries()]
      .filter(([, list]) => list.length > 0)
      .map(([region, list]) => ({ region, channels: list.map((c) => ({ id: c.id, players: c.count })) }));
    return {
      players: this.where.size,
      sharedChannels: this.channels.size - this.privateCount,
      privateInstances: this.privateCount,
      regions,
      lastTickMs: Math.round(this.lastTickMs * 100) / 100,
    };
  }

  /** Total player slots the server can hold right now with the configured budgets. */
  get capacity(): number {
    return this.cfg.maxChannels * this.cfg.channelHardCap;
  }

  // ---------------------------------------------------------------- internals

  private place(ch: Channel, link: PlayerLink, at: Vec3, change?: ArrivalInfo & { via: TransferVia; from: string }, anchor = 0): void {
    ch.add(link, at, anchor);
    this.where.set(link.heroId, ch);
    if (!change) this.events.emit({ type: 'player_connected', heroId: link.heroId, channelId: ch.id });
    this.events.emit({ type: 'region_enter', heroId: link.heroId, region: ch.region.name, channelId: ch.id, private: ch.kind === 'private' });
    // The sim may have snapped the hero onto its ground; tell the client where they really are.
    const p = ch.sim.position(link.heroId) ?? at;
    const msg: Record<string, unknown> = { t: 'enter_region', region: ch.region.name, region_uid: ch.region.uid, channel: ch.id, x: p.x, y: p.y, z: p.z };
    if (change) {
      msg.from = change.from;
      msg.via = change.via;
      msg.loading = change.via !== 'client';
      if (change.entrance) msg.entrance = change.entrance;
      if (change.thing) msg.thing = change.thing;
      if (change.yaw !== undefined) msg.yaw = change.yaw;
    }
    link.send(JSON.stringify(msg));
    this.hooks.onEnter?.(ch, link);
    if (this.clock) link.send(JSON.stringify({ t: 'sky', ...this.clock.sky(ch.region.name) }));
  }

  /**
   * Tell a channel's players about the sky when the weather or day/night changes, and every
   * SKY_RESYNC_MS anyway so clients that drifted (a paused game, a slow machine) snap back.
   */
  private syncSky(ch: Channel, now: number): void {
    const key = this.clock!.skyKey(ch.region.name, now);
    if (key === ch.skyKey && now - ch.skyAt < SKY_RESYNC_MS) return;
    ch.skyKey = key;
    ch.skyAt = now;
    if (ch.count) ch.broadcast(JSON.stringify({ t: 'sky', ...this.clock!.sky(ch.region.name, now) }));
  }

  private pickShared(region: string): Channel {
    const list = this.shared.get(region) ?? [];
    let best: Channel | undefined;
    for (const c of list) if (c.count < this.cfg.channelSoftCap && (!best || c.count > best.count)) best = c;
    if (best) return best;
    if (this.sharedCount() < this.cfg.maxChannels) return this.openShared(region);
    for (const c of list) if (c.count < this.cfg.channelHardCap && (!best || c.count < best.count)) best = c;
    if (best) return best;
    throw new GameError('region_full', 'That region is full, try again shortly', 503);
  }

  private openShared(region: string): Channel {
    const def = this.regions.get(region)!;
    const n = (this.nextIndex.get(region) ?? 0) + 1;
    this.nextIndex.set(region, n);
    const id = `${region}#${n}`;
    const ch = new Channel(id, def, 'shared', this.simFactory(def, id), undefined, this.cfg.interestRadius);
    this.channels.set(id, ch);
    this.shared.set(region, [...(this.shared.get(region) ?? []), ch]);
    this.events.emit({ type: 'channel_open', channelId: id, region, private: false });
    return ch;
  }

  private openPrivate(region: string, heroId: number): Channel {
    if (this.privateCount >= this.cfg.maxPrivateInstances)
      throw new GameError('instances_full', 'All solo instances are busy, try again shortly', 503);
    const def = this.regions.get(region)!;
    const id = `${region}~${heroId}`;
    const ch = new Channel(id, def, 'private', this.simFactory(def, id), heroId, this.cfg.interestRadius);
    this.channels.set(id, ch);
    this.privateCount++;
    this.events.emit({ type: 'channel_open', channelId: id, region, private: true });
    return ch;
  }

  private closeChannel(ch: Channel): void {
    if (!this.channels.delete(ch.id)) return;
    if (ch.kind === 'private') this.privateCount--;
    else {
      const list = (this.shared.get(ch.region.name) ?? []).filter((c) => c !== ch);
      if (list.length) this.shared.set(ch.region.name, list);
      else this.shared.delete(ch.region.name);
    }
    ch.sim.dispose();
    this.hooks.onClose?.(ch);
    this.events.emit({ type: 'channel_close', channelId: ch.id, region: ch.region.name, lifetimeMs: Date.now() - ch.openedAt });
  }

  private sharedCount(): number {
    return this.channels.size - this.privateCount;
  }
}

const isOrigin = (p: Vec3) => p.x === 0 && p.y === 0 && p.z === 0;
