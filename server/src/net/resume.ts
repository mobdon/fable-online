import crypto from 'node:crypto';

const PREFIX = 'resume.';

/**
 * Lets a game connection that dropped come back without a new launcher ticket. Join tickets
 * are one-time, so without this a Wi-Fi blip would mean quitting Fable and relaunching.
 *
 * Every `welcome` carries a fresh token for that hero. It is good once, for `ttlMs` after the
 * connection that got it ends (and for as long as that connection is up), and only on this
 * realm. Issuing a new one for a hero cancels the old one. Kept in memory on purpose: a
 * server restart cancels them all, and players go back through the launcher.
 */
export class ResumeTokens {
  private byToken = new Map<string, { heroId: number; accountId: number; until: number }>();
  private byHero = new Map<number, string>();

  constructor(private ttlMs: number) {}

  static looksLike(token: string): boolean {
    return token.startsWith(PREFIX);
  }

  issue(heroId: number, accountId: number): string {
    this.cancel(heroId);
    const token = PREFIX + crypto.randomBytes(32).toString('base64url');
    this.byToken.set(token, { heroId, accountId, until: Infinity });
    this.byHero.set(heroId, token);
    return token;
  }

  /** The connection holding the hero's token ended: start its clock. */
  disconnected(heroId: number, now = Date.now()): void {
    const t = this.byHero.get(heroId);
    const e = t ? this.byToken.get(t) : undefined;
    if (e) e.until = now + this.ttlMs;
  }

  /** Uses the token up. Returns who it was for, or null if it is unknown, used or stale. */
  redeem(token: string, now = Date.now()): { heroId: number; accountId: number } | null {
    const e = this.byToken.get(token);
    if (!e) return null;
    this.byToken.delete(token);
    if (this.byHero.get(e.heroId) === token) this.byHero.delete(e.heroId);
    if (now > e.until) return null;
    return { heroId: e.heroId, accountId: e.accountId };
  }

  cancel(heroId: number): void {
    const t = this.byHero.get(heroId);
    if (t) this.byToken.delete(t);
    this.byHero.delete(heroId);
  }

  /** Drops stale tokens. Called now and then; redeem checks expiry itself anyway. */
  sweep(now = Date.now()): void {
    for (const [t, e] of this.byToken)
      if (now > e.until) {
        this.byToken.delete(t);
        if (this.byHero.get(e.heroId) === t) this.byHero.delete(e.heroId);
      }
  }

  get size(): number {
    return this.byToken.size;
  }
}
