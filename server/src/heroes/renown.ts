import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';

/** The highest renown a report may name. Anything above is not a number Fable can reach. */
export const MAX_REPORTED_RENOWN = 10_000_000;

export interface RenownRules {
  /** Taken once, from the first report: the renown the hero earned before the server saw them. */
  importMax: number;
  /** After the import, the most reports may add per hero per UTC day. */
  dailyMax: number;
}

export type RenownResult =
  | { ok: true; renown: number; gained: number; kind: 'import' | 'daily'; capped: boolean }
  | { ok: false; reason: 'no_hero' | 'bad_total' };

/**
 * Renown as the server keeps it. Quests (the main source of renown) run on each player's own
 * Fable, so the mod reads the hero's total renown there (CTCHeroStats RenownTotal) and
 * reports it. The report is a number a cheat can choose, so:
 *  - the first report for a hero is the one-off import of what they earned before, capped
 *    at `importMax`;
 *  - after that, a report only ever raises the server's renown, by at most `dailyMax` per
 *    UTC day across all reports;
 *  - a lower report changes nothing: the server's number never drops because of a client.
 * The report is the total in Fable, not a delta, so quest claims that already added renown
 * on the server are not counted twice: the gap between the two is what is left to add.
 */
export class Renown {
  constructor(
    private db: Database,
    private events: EventSink,
    private rules: RenownRules,
  ) {}

  report(heroId: number, total: number, now = Date.now()): RenownResult {
    if (!Number.isSafeInteger(total) || total < 0 || total > MAX_REPORTED_RENOWN) return { ok: false, reason: 'bad_total' };
    return this.db.tx(() => {
      const h = this.db.get<{ renown: number; renown_imported_at: number | null }>(
        'SELECT renown, renown_imported_at FROM heroes WHERE id = ?',
        heroId,
      );
      if (!h) return { ok: false as const, reason: 'no_hero' as const };
      let gained = 0;
      let capped = false;
      let kind: 'import' | 'daily';
      if (h.renown_imported_at == null) {
        kind = 'import';
        const take = Math.min(total, Math.max(0, this.rules.importMax));
        capped = take < total;
        gained = Math.max(0, take - h.renown);
        this.db.run('UPDATE heroes SET renown_imported_at = ? WHERE id = ?', now, heroId);
      } else {
        kind = 'daily';
        const want = total - h.renown;
        if (want > 0) {
          const day = new Date(now).toISOString().slice(0, 10);
          const row = this.db.get<{ gained: number }>('SELECT gained FROM renown_days WHERE hero_id = ? AND day = ?', heroId, day);
          const left = Math.max(0, this.rules.dailyMax - (row?.gained ?? 0));
          gained = Math.min(want, left);
          capped = gained < want;
          if (gained > 0)
            this.db.run(
              'INSERT INTO renown_days (hero_id, day, gained) VALUES (?, ?, ?) ON CONFLICT (hero_id, day) DO UPDATE SET gained = gained + excluded.gained',
              heroId,
              day,
              gained,
            );
        }
      }
      if (gained > 0) this.db.run('UPDATE heroes SET renown = renown + ? WHERE id = ?', gained, heroId);
      const renown = h.renown + gained;
      if (gained > 0 || capped || kind === 'import')
        this.events.emit({ type: 'renown_report', heroId, reported: total, gained, renown, kind, capped });
      return { ok: true as const, renown, gained, kind, capped };
    });
  }
}
