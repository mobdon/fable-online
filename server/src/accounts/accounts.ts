import crypto from 'node:crypto';
import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import { GameError } from '../errors.js';

const SCRYPT = { N: 16384, r: 8, p: 1, keylen: 32 };
const USERNAME_RE = /^[A-Za-z0-9_]{3,20}$/;

export interface Account {
  id: number;
  username: string;
  role: 'player' | 'gm' | 'admin';
  banned_until: number | null;
}

export interface TokenPair {
  access_token: string;
  refresh_token: string;
  expires_in: number; // seconds
  account: { id: number; username: string; role: string };
}

const sha256 = (s: string) => crypto.createHash('sha256').update(s).digest('hex');
const newToken = () => crypto.randomBytes(32).toString('base64url');

function hashPassword(password: string): string {
  const salt = crypto.randomBytes(16);
  const hash = crypto.scryptSync(password, salt, SCRYPT.keylen, SCRYPT);
  return `scrypt$${SCRYPT.N}$${salt.toString('base64')}$${hash.toString('base64')}`;
}

function verifyPassword(password: string, stored: string): boolean {
  const [algo, n, salt, hash] = stored.split('$');
  if (algo !== 'scrypt' || !n || !salt || !hash) return false;
  const expected = Buffer.from(hash, 'base64');
  const got = crypto.scryptSync(password, Buffer.from(salt, 'base64'), expected.length, { ...SCRYPT, N: Number(n) });
  return crypto.timingSafeEqual(expected, got);
}

/** Simple fixed-window limiter for login attempts, keyed by username and by IP. */
class AttemptLimiter {
  private hits = new Map<string, { n: number; until: number }>();
  constructor(private max: number, private windowMs: number) {}
  check(key: string, now = Date.now()): boolean {
    const h = this.hits.get(key);
    if (!h || h.until < now) {
      this.hits.set(key, { n: 1, until: now + this.windowMs });
      return true;
    }
    h.n++;
    return h.n <= this.max;
  }
}

export const ACCESS_TTL_MS = 15 * 60_000;
export const JOIN_TICKET_TTL_MS = 60_000;

export class Accounts {
  private limiter = new AttemptLimiter(10, 60_000);

  constructor(
    private db: Database,
    private events: EventSink,
    private refreshTtlMs: number,
  ) {}

  register(username: string, password: string): Account {
    if (!USERNAME_RE.test(username)) throw new GameError('bad_username', 'Username must be 3-20 letters, digits or _');
    if (typeof password !== 'string' || password.length < 8 || password.length > 128)
      throw new GameError('bad_password', 'Password must be 8-128 characters');
    if (this.db.get('SELECT 1 FROM accounts WHERE username = ?', username))
      throw new GameError('username_taken', 'That username is taken', 409);
    const { lastInsertRowid: id } = this.db.run(
      'INSERT INTO accounts (username, password_hash, created_at) VALUES (?, ?, ?)',
      username,
      hashPassword(password),
      Date.now(),
    );
    this.events.emit({ type: 'account_registered', accountId: id });
    return this.byId(id)!;
  }

  byId(id: number): Account | undefined {
    return this.db.get<Account>('SELECT id, username, role, banned_until FROM accounts WHERE id = ?', id);
  }

  login(username: string, password: string, ip = 'unknown'): TokenPair {
    if (!this.limiter.check(`u:${String(username).toLowerCase()}`) || !this.limiter.check(`ip:${ip}`))
      throw new GameError('rate_limited', 'Too many attempts, wait a minute', 429);
    const row = this.db.get<Account & { password_hash: string }>(
      'SELECT id, username, role, banned_until, password_hash FROM accounts WHERE username = ?',
      String(username),
    );
    if (!row || !verifyPassword(String(password), row.password_hash)) {
      if (row) this.events.emit({ type: 'login', accountId: row.id, ok: false, reason: 'bad_password' });
      throw new GameError('bad_credentials', 'Wrong username or password', 401);
    }
    if (row.banned_until && row.banned_until > Date.now()) {
      this.events.emit({ type: 'login', accountId: row.id, ok: false, reason: 'banned' });
      throw new GameError('banned', 'This account is suspended', 403);
    }
    this.db.run('UPDATE accounts SET last_login_at = ? WHERE id = ?', Date.now(), row.id);
    this.events.emit({ type: 'login', accountId: row.id, ok: true });
    return this.issuePair(row);
  }

  /** Rotating refresh: the old refresh token is consumed, a new pair is issued. */
  refresh(refreshToken: string): TokenPair {
    return this.db.tx(() => {
      const s = this.consume(refreshToken, 'refresh');
      const acct = this.byId(s.account_id);
      if (!acct) throw new GameError('bad_token', 'Session expired', 401);
      return this.issuePair(acct);
    });
  }

  logout(accessToken: string): void {
    const acct = this.authenticate(accessToken);
    this.db.run("DELETE FROM sessions WHERE account_id = ? AND kind IN ('access','refresh')", acct.id);
  }

  /** Resolve an access token to its account, or throw 401. */
  authenticate(accessToken: string | undefined): Account {
    if (!accessToken) throw new GameError('no_token', 'Not logged in', 401);
    const s = this.db.get<{ account_id: number; expires_at: number }>(
      "SELECT account_id, expires_at FROM sessions WHERE token_hash = ? AND kind = 'access'",
      sha256(accessToken),
    );
    if (!s || s.expires_at < Date.now()) throw new GameError('bad_token', 'Session expired', 401);
    const acct = this.byId(s.account_id);
    if (!acct || (acct.banned_until && acct.banned_until > Date.now()))
      throw new GameError('bad_token', 'Session expired', 401);
    return acct;
  }

  /** One-time ticket the client mod presents in Hello.token. Valid for 60 s, single use. */
  issueJoinTicket(accountId: number, heroId: number, realm: string): string {
    const t = newToken();
    this.db.run(
      "INSERT INTO sessions (token_hash, account_id, kind, hero_id, realm, expires_at, created_at) VALUES (?, ?, 'join', ?, ?, ?, ?)",
      sha256(t),
      accountId,
      heroId,
      realm,
      Date.now() + JOIN_TICKET_TTL_MS,
      Date.now(),
    );
    return t;
  }

  redeemJoinTicket(ticket: string): { accountId: number; heroId: number; realm: string } {
    const s = this.db.tx(() => this.consume(ticket, 'join'));
    return { accountId: s.account_id, heroId: s.hero_id!, realm: s.realm! };
  }

  private consume(token: string, kind: 'refresh' | 'join') {
    const h = sha256(String(token));
    const s = this.db.get<{ account_id: number; hero_id: number | null; realm: string | null; expires_at: number }>(
      'SELECT account_id, hero_id, realm, expires_at FROM sessions WHERE token_hash = ? AND kind = ?',
      h,
      kind,
    );
    // Delete first: even an expired or replayed token is gone after one attempt.
    this.db.run('DELETE FROM sessions WHERE token_hash = ?', h);
    if (!s || s.expires_at < Date.now()) throw new GameError('bad_token', 'Ticket or session expired', 401);
    return s;
  }

  private issuePair(acct: Account): TokenPair {
    const now = Date.now();
    const access = newToken();
    const refresh = newToken();
    this.db.run('DELETE FROM sessions WHERE expires_at < ?', now);
    const ins = 'INSERT INTO sessions (token_hash, account_id, kind, expires_at, created_at) VALUES (?, ?, ?, ?, ?)';
    this.db.run(ins, sha256(access), acct.id, 'access', now + ACCESS_TTL_MS, now);
    this.db.run(ins, sha256(refresh), acct.id, 'refresh', now + this.refreshTtlMs, now);
    return {
      access_token: access,
      refresh_token: refresh,
      expires_in: ACCESS_TTL_MS / 1000,
      account: { id: acct.id, username: acct.username, role: acct.role },
    };
  }
}
