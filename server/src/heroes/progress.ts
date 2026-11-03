import type { Database } from '../db/database.js';

export interface HeroProgress {
  hero_id: number;
  experience: number;
  kills: number;
  deaths: number;
  pvp_kills: number;
}

/** Combat progress the server saw happen. Experience is never taken from a client. */
export class Progress {
  constructor(private db: Database) {}

  get(heroId: number): HeroProgress {
    return (
      this.db.get<HeroProgress>('SELECT * FROM hero_progress WHERE hero_id = ?', heroId) ?? { hero_id: heroId, experience: 0, kills: 0, deaths: 0, pvp_kills: 0 }
    );
  }

  private bump(heroId: number, col: 'kills' | 'deaths' | 'pvp_kills', xp = 0): void {
    if (!this.db.get('SELECT 1 FROM heroes WHERE id = ?', heroId)) return;
    this.db.run(
      `INSERT INTO hero_progress (hero_id, experience, ${col}) VALUES (?, ?, 1)
       ON CONFLICT(hero_id) DO UPDATE SET experience = experience + excluded.experience, ${col} = ${col} + 1`,
      heroId,
      Math.max(0, Math.floor(xp)),
    );
  }

  kill(heroId: number, experience: number): void {
    this.bump(heroId, 'kills', experience);
  }
  death(heroId: number): void {
    this.bump(heroId, 'deaths');
  }
  pvpKill(heroId: number): void {
    this.bump(heroId, 'pvp_kills');
  }
}
