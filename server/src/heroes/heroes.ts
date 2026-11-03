import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';
import { STAGE_REGIONS } from '../world/regions.js';

export type HeroPhase = 'childhood' | 'guild_training' | 'world';

export interface Hero {
  id: number;
  account_id: number;
  name: string;
  phase: HeroPhase;
  pvp_unlocked: number;
  renown: number;
  region: string;
  x: number;
  y: number;
  z: number;
  created_at: number;
  last_played_at: number | null;
}

const NAME_RE = /^[A-Z][a-z]{2,15}$/; // Fable-style single names: "Sparrow", "Thorne"
const PHASE_ORDER: HeroPhase[] = ['childhood', 'guild_training', 'world'];

export class Heroes {
  constructor(
    private db: Database,
    private events: EventSink,
    private maxPerAccount: number,
  ) {}

  create(accountId: number, name: string): Hero {
    if (!NAME_RE.test(name)) throw new GameError('bad_name', 'Name must be one word, 3-16 letters, capitalised');
    return this.db.tx(() => {
      const { n } = this.db.get<{ n: number }>('SELECT COUNT(*) AS n FROM heroes WHERE account_id = ?', accountId)!;
      if (n >= this.maxPerAccount) throw new GameError('hero_limit', `At most ${this.maxPerAccount} heroes per account`);
      if (this.db.get('SELECT 1 FROM heroes WHERE name = ?', name)) throw new GameError('name_taken', 'That name is taken', 409);
      const { lastInsertRowid: id } = this.db.run(
        'INSERT INTO heroes (account_id, name, region, created_at) VALUES (?, ?, ?, ?)',
        accountId,
        name,
        STAGE_REGIONS.childhood,
        Date.now(),
      );
      // Every hero gets exactly one wallet, created with the hero, starting at 0.
      this.db.run("INSERT INTO wallets (id, kind) VALUES (?, 'hero')", `hero:${id}`);
      this.events.emit({ type: 'hero_created', accountId, heroId: id });
      return this.get(id)!;
    });
  }

  get(id: number): Hero | undefined {
    return this.db.get<Hero>('SELECT * FROM heroes WHERE id = ?', id);
  }

  listForAccount(accountId: number): Hero[] {
    return this.db.all<Hero>(
      'SELECT * FROM heroes WHERE account_id = ? ORDER BY COALESCE(last_played_at, created_at) DESC',
      accountId,
    );
  }

  /** The account's hero, or its most recently played one when heroId is omitted. */
  pick(accountId: number, heroId?: number): Hero {
    const hero = heroId === undefined ? this.listForAccount(accountId)[0] : this.get(heroId);
    if (!hero || hero.account_id !== accountId) throw new GameError('no_hero', 'Hero not found', 404);
    return hero;
  }

  /** Server-authoritative position write. Only the region simulation calls this. */
  savePosition(id: number, region: string, x: number, y: number, z: number): void {
    this.db.run('UPDATE heroes SET region = ?, x = ?, y = ?, z = ?, last_played_at = ? WHERE id = ?', region, x, y, z, Date.now(), id);
  }

  /** Life stages only move forward, one step at a time. Called by quest scripts on the server. */
  advancePhase(id: number, to: HeroPhase): Hero {
    const hero = this.get(id);
    if (!hero) throw new GameError('no_hero', 'Hero not found', 404);
    if (PHASE_ORDER.indexOf(to) !== PHASE_ORDER.indexOf(hero.phase) + 1)
      throw new GameError('bad_phase', `Cannot go from ${hero.phase} to ${to}`);
    this.db.run('UPDATE heroes SET phase = ?, region = ?, x = 0, y = 0, z = 0 WHERE id = ?', to, STAGE_REGIONS[to], id);
    return this.get(id)!;
  }

  /** Set when the hero completes the custom Guild mission that appears on first reaching Oakvale. */
  unlockPvp(id: number): void {
    this.db.run("UPDATE heroes SET pvp_unlocked = 1 WHERE id = ? AND phase = 'world'", id);
  }
}
