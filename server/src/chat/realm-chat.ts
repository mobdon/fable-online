import { randomBytes } from 'node:crypto';
import type { Server } from 'node:net';
import type { ServerConfig } from '../config.js';
import type { Director } from '../world/director.js';
import type { Heroes, Hero } from '../heroes/heroes.js';
import type { Account } from '../accounts/accounts.js';
import { ChatHub, type EventSink as ChatEvents } from './hub.js';
import { signToken, type Role } from './auth.js';
import { listen } from './server.js';

/** How often name tags and chat positions follow the world. */
const SYNC_MS = 200;
/** How often a hero's renown and PvP flag are read again from the database. */
const HERO_REFRESH_MS = 5_000;

/**
 * Realm chat inside the game server. The ChatHub keeps its own TCP port (the mod's chat
 * client speaks NDJSON, not the world socket's protocol), but logins only work with a token
 * this server signed in /launcher/v1/join, and positions, regions, renown and PvP flags come
 * from the world, never from the client.
 */
export class RealmChat {
  readonly hub: ChatHub;
  private readonly secret: string;
  private readonly heroCache = new Map<number, { name: string; renown: number; pvp: boolean; at: number }>();
  private lastSync = 0;
  private server: Server | null = null;

  constructor(
    private cfg: ServerConfig,
    private director: Director,
    private heroes: Heroes,
    emit: ChatEvents,
    now: () => number = Date.now,
  ) {
    // A secret made at boot is fine for one process: tokens only live as long as a join.
    // Set FO_CHAT_SECRET once chat moves to its own process or several servers share it.
    this.secret = cfg.chatSecret || randomBytes(32).toString('base64url');
    this.hub = new ChatHub(
      {
        tokenSecret: this.secret,
        trustClientPositions: false,
        motd: cfg.chatMotd || undefined,
        bannedWords: cfg.chatBannedWords,
        moderationFile: cfg.chatModerationPath || null,
      },
      emit,
      now,
    );
    director.policies.push({
      tick: (_dt, ids) => this.sync(ids),
      left: (heroId) => this.heroCache.delete(heroId),
    });
  }

  /** The chat login for one game session. Its life matches the launcher session's. */
  token(account: Pick<Account, 'id' | 'role'>, hero: Hero, nowMs = Date.now()): string {
    return signToken(
      {
        sub: String(account.id),
        name: hero.name,
        role: chatRole(account.role),
        renown: hero.renown,
        exp: Math.floor((nowMs + this.cfg.chatTokenTtlMs) / 1000),
      },
      this.secret,
    );
  }

  /** What /join hands the launcher, which passes it to the mod. */
  joinInfo(account: Pick<Account, 'id' | 'role'>, hero: Hero): { host: string; port: number; token: string } {
    return { host: this.cfg.chatPublicHost || this.cfg.publicHost, port: this.cfg.chatPublicPort, token: this.token(account, hero) };
  }

  async listen(): Promise<Server> {
    this.server = await listen(this.hub, this.cfg.chatPort, this.cfg.chatHost);
    return this.server;
  }

  close(): void {
    this.server?.close();
    this.server = null;
  }

  /** Copy every online hero's place in the world into the hub, then send name tags. */
  sync(heroIds: Iterable<number>, now = Date.now()): void {
    if (now - this.lastSync < SYNC_MS) return;
    this.lastSync = now;
    for (const id of heroIds) {
      const ch = this.director.channelOf(id);
      const hero = this.hero(id, now);
      if (!ch || !hero) continue;
      const pos = ch.sim.position(id);
      this.hub.updateHero(hero.name, {
        region: regionKey(ch.id),
        area: ch.areaOf(id),
        renown: hero.renown,
        pvp: hero.pvp,
        ...(pos ? { x: pos.x, y: pos.y, z: pos.z } : {}),
      });
    }
    this.hub.tick();
  }

  private hero(id: number, now: number) {
    const c = this.heroCache.get(id);
    if (c && now - c.at < HERO_REFRESH_MS) return c;
    const h = this.heroes.get(id);
    if (!h) return undefined;
    const fresh = { name: h.name, renown: h.renown, pvp: this.cfg.pvpEverywhere || !!h.pvp_unlocked, at: now };
    this.heroCache.set(id, fresh);
    return fresh;
  }
}

/** Account roles to chat roles: GMs moderate chat. */
export function chatRole(role: string): Role {
  return role === 'admin' ? 'admin' : role === 'gm' ? 'mod' : 'player';
}

/**
 * The hub groups heroes by a number. Each channel (one copy of one region) gets its own, so
 * region chat and name tags stay inside the copy a hero is in. FNV-1a of the channel id.
 */
export function regionKey(channelId: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < channelId.length; i++) {
    h ^= channelId.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}
