import crypto from 'node:crypto';
import type { Database } from '../db/database.js';
import { GameError } from '../errors.js';
import { CDKEY, LOGIN_KEY, lhDecrypt, lhEncrypt } from './lhcrypt.js';
import { hasCrLf, makeLogo } from './logo.js';

/**
 * Fable's own online scoreboard and clan screens, answered by our server.
 *
 * Fable.exe still has the full client: it POSTs to <host>/index.php with an
 * X-LH-Username header (plus "X-LH-Login: Login" on the first call). The body is a
 * CR LF separated text message, encrypted with lhcrypt:
 *
 *   Fable1.5 / Version 1 / <ACTION> / <username> / <md5 or session> / <params...>
 *
 * Login uses LOGIN_KEY, every later call uses CDKEY, and the reply is encrypted with the
 * same key as its request. Every reply after login starts with "SID:OK".
 * The full format is in tools/re-notes/07-scoreboard-protocol.md.
 */

export const ACTIVITY_COUNT = 39;
/** Top rows per activity in one scoreboard download. */
const ROWS_PER_ACTIVITY = 20;
const SESSION_TTL_MS = 6 * 3600_000;
const CLAN_NAME_RE = /^[A-Za-z0-9 '_-]{3,32}$/;
export const WORLD_CLAN_ID = 1;

export interface FableReply {
  status: number;
  body: Buffer;
}

interface ClanRow {
  id: number;
  name: string;
  description: string | null;
  logo_version: number;
  logo_jpeg: Uint8Array | null;
}

const md5 = (s: string) => crypto.createHash('md5').update(s, 'latin1').digest('hex');
/** The clan screen turns "''" back into "'", like an SQL literal. */
const esc = (s: string) => s.replace(/[\r\n]/g, ' ').replace(/'/g, "''");
const SID_OK = 'SID:OK';

/** Split a decrypted message on CR LF (binary safe: lines stay latin1). */
export function splitLines(b: Buffer): string[] {
  const out: string[] = [];
  let from = 0;
  for (;;) {
    const i = b.indexOf('\r\n', from, 'latin1');
    if (i < 0) break;
    out.push(b.subarray(from, i).toString('latin1'));
    from = i + 2;
  }
  if (from < b.length) out.push(b.subarray(from).toString('latin1'));
  return out;
}

export const joinLines = (lines: (string | Buffer)[]) =>
  Buffer.concat(lines.flatMap((l) => [typeof l === 'string' ? Buffer.from(l, 'latin1') : l, Buffer.from('\r\n')]));

export class Scoreboard {
  private sessions = new Map<string, { accountId: number; username: string; until: number }>();
  /** Last few raw requests, for the "capture one real request" step of testing. */
  readonly recent: { at: number; action: string; username: string; lines: string[] }[] = [];

  constructor(private db: Database) {}

  // ---------------------------------------------------------------- admin and launcher side

  /** New scoreboard code for an account. The player types it as the password in Fable's scoreboard login. */
  rotateCode(accountId: number): { username: string; code: string } {
    const acct = this.db.get<{ username: string }>('SELECT username FROM accounts WHERE id = ?', accountId);
    if (!acct) throw new GameError('no_account', 'No such account', 404);
    const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    const code = Array.from(crypto.randomBytes(10), (b) => alphabet[b % alphabet.length]).join('');
    this.db.run(
      `INSERT INTO scoreboard_logins (account_id, md5, created_at) VALUES (?, ?, ?)
       ON CONFLICT(account_id) DO UPDATE SET md5 = excluded.md5, created_at = excluded.created_at`,
      accountId,
      md5((acct.username + code).toLowerCase()),
      Date.now(),
    );
    return { username: acct.username, code };
  }

  createClan(name: string, description = ''): ClanRow {
    if (!CLAN_NAME_RE.test(name)) throw new GameError('bad_clan_name', 'Clan name must be 3-32 letters, digits, spaces, _ - or \'');
    if (this.db.get('SELECT 1 FROM clans WHERE name = ?', name)) throw new GameError('clan_taken', 'That clan name is taken', 409);
    const { lastInsertRowid } = this.db.run(
      'INSERT INTO clans (name, description, created_at) VALUES (?, ?, ?)',
      name,
      description.slice(0, 200) || null,
      Date.now(),
    );
    return this.clan(Number(lastInsertRowid))!;
  }

  clan(id: number): ClanRow | undefined {
    return this.db.get<ClanRow>('SELECT * FROM clans WHERE id = ?', id);
  }

  clanByName(name: string): ClanRow | undefined {
    return this.db.get<ClanRow>('SELECT * FROM clans WHERE name = ?', name.trim());
  }

  addMember(clanId: number, accountId: number): void {
    if (!this.clan(clanId)) throw new GameError('no_clan', 'No such clan', 404);
    this.db.run('INSERT OR IGNORE INTO clan_members (clan_id, account_id, joined_at) VALUES (?, ?, ?)', clanId, accountId, Date.now());
  }

  removeMember(clanId: number, accountId: number): void {
    if (clanId === WORLD_CLAN_ID) throw new GameError('world_clan', 'Everyone stays in the realm clan');
    this.db.run('DELETE FROM clan_members WHERE clan_id = ? AND account_id = ?', clanId, accountId);
  }

  setLogo(clanId: number, jpegBytes: Buffer): { version: number; bytes: number } {
    const logo = makeLogo(jpegBytes);
    return this.db.tx(() => {
      const c = this.clan(clanId);
      if (!c) throw new GameError('no_clan', 'No such clan', 404);
      const version = c.logo_version + 1;
      this.db.run('UPDATE clans SET logo_version = ?, logo_jpeg = ? WHERE id = ?', version, logo, clanId);
      return { version, bytes: logo.length };
    });
  }

  // ---------------------------------------------------------------- the game's side

  /** Answer one POST /index.php from Fable.exe. Never throws: bad input gets an empty 400. */
  handle(headers: Record<string, string | string[] | undefined>, body: Buffer, now = Date.now()): FableReply {
    const isLogin = headers['x-lh-login'] !== undefined;
    const key = isLogin ? LOGIN_KEY : CDKEY;
    let lines: string[];
    try {
      lines = splitLines(lhDecrypt(body, key));
    } catch {
      return { status: 400, body: Buffer.alloc(0) };
    }
    const [, , action = '', username = ''] = lines;
    this.recent.push({ at: now, action, username, lines: lines.slice(0, 6).map((l) => (/^[\x20-\x7e]*$/.test(l) ? l : `<${l.length} bytes>`)) });
    if (this.recent.length > 20) this.recent.shift();

    const reply = (out: (string | Buffer)[]): FableReply => ({ status: 200, body: lhEncrypt(joinLines(out), key) });

    if (action === 'LOG_ON_TO_REG_SERVER') return reply(this.login(username, lines[4] ?? '', now));

    const s = this.sessions.get(lines[4] ?? '');
    if (!s || s.until < now || s.username.toLowerCase() !== username.toLowerCase()) return reply(['SID:EXPIRED']);
    s.until = now + SESSION_TTL_MS;
    const params = lines.slice(5);
    switch (action) {
      case 'DOWNLOAD_CLAN_LIST':
        return reply([SID_OK, ...this.clanList(s.accountId)]);
      case 'DOWNLOAD_CLAN_LOGOS':
        return reply([SID_OK, ...this.clanLogos(s.accountId, params)]);
      case 'DOWNLOAD_MEMBER_LIST':
        return reply([SID_OK, ...this.memberList(params[0] ?? '')]);
      case 'DOWNLOAD_MEMBER_LOGOS':
        return reply([SID_OK]); // no per-member pictures yet
      case 'DOWNLOAD_SCOREBOARD':
        return reply([SID_OK, ...this.board(params[0] ?? '')]);
      case 'UPLOAD_SCORES':
        this.upload(s.accountId, params, now);
        return reply([SID_OK]);
      default:
        return reply(['SID:UNKNOWN_ACTION']);
    }
  }

  /** Reply lines: <ignored>, <status>, <session id encrypted with CDKEY>. Status 0 is success, -3 is "login failed". */
  private login(username: string, hash: string, now: number): (string | Buffer)[] {
    const fail = ['SID:FAIL', '-3'];
    const acct = this.db.get<{ id: number; username: string; banned_until: number | null; md5: string | null }>(
      `SELECT a.id, a.username, a.banned_until, s.md5 FROM accounts a
       LEFT JOIN scoreboard_logins s ON s.account_id = a.id WHERE a.username = ?`,
      username,
    );
    if (!acct || !acct.md5 || (acct.banned_until && acct.banned_until > now)) return fail;
    const a = Buffer.from(acct.md5, 'latin1');
    const b = Buffer.from(hash.toLowerCase(), 'latin1');
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return fail;

    this.addMember(WORLD_CLAN_ID, acct.id);
    for (const [k, v] of this.sessions) if (v.until < now) this.sessions.delete(k);
    // The game reads the session back as a C string, so it carries its own NUL, and the
    // encrypted form must not contain CR LF (the reply is split on it).
    for (;;) {
      const sid = crypto.randomBytes(12).toString('hex');
      const blob = lhEncrypt(Buffer.from(sid + '\0', 'latin1'), CDKEY);
      if (hasCrLf(blob)) continue;
      this.sessions.set(sid, { accountId: acct.id, username: acct.username, until: now + SESSION_TTL_MS });
      return [SID_OK, '0', blob];
    }
  }

  private clansOf(accountId: number): ClanRow[] {
    return this.db.all<ClanRow>(
      `SELECT c.* FROM clans c JOIN clan_members m ON m.clan_id = c.id WHERE m.account_id = ? ORDER BY c.id`,
      accountId,
    );
  }

  /** Per clan: name, id, description (or NO_DESCRIPTION). */
  private clanList(accountId: number): string[] {
    return this.clansOf(accountId).flatMap((c) => [esc(c.name), String(c.id), c.description ? esc(c.description) : 'NO_DESCRIPTION']);
  }

  /**
   * The game sends "<count>" then "<clan id>" "<logo version on disk>" pairs. Reply per logo it
   * lacks: clan id, version, byte length, raw JPEG. It saves that as ClanData\<id>\logo.<version>.jpg.
   */
  private clanLogos(accountId: number, params: string[]): (string | Buffer)[] {
    const have = new Map<number, number>();
    const n = Math.min(Number.parseInt(params[0] ?? '0', 10) || 0, 64);
    for (let i = 0; i < n; i++) {
      const id = Number.parseInt(params[1 + i * 2] ?? '', 10);
      const ver = Number.parseInt(params[2 + i * 2] ?? '', 10);
      if (Number.isFinite(id)) have.set(id, Number.isFinite(ver) ? ver : -1);
    }
    const out: (string | Buffer)[] = [];
    for (const c of this.clansOf(accountId)) {
      if (!c.logo_jpeg || c.logo_version <= 0) continue;
      if ((have.get(c.id) ?? -1) >= c.logo_version) continue;
      const bytes = Buffer.from(c.logo_jpeg);
      out.push(String(c.id), String(c.logo_version), String(bytes.length), bytes);
    }
    return out;
  }

  private findClan(param: string): ClanRow | undefined {
    const t = param.trim();
    return this.clanByName(t) ?? (/^\d+$/.test(t) ? this.clan(Number(t)) : undefined);
  }

  /** Per member: name, id, description. */
  private memberList(clanParam: string): string[] {
    const c = this.findClan(clanParam);
    if (!c) return [];
    const rows = this.db.all<{ id: number; username: string; hero: string | null; renown: number | null }>(
      `SELECT a.id, a.username,
              (SELECT name FROM heroes h WHERE h.account_id = a.id ORDER BY h.renown DESC LIMIT 1) AS hero,
              (SELECT MAX(renown) FROM heroes h WHERE h.account_id = a.id) AS renown
       FROM clan_members m JOIN accounts a ON a.id = m.account_id
       WHERE m.clan_id = ? ORDER BY a.username LIMIT 500`,
      c.id,
    );
    return rows.flatMap((r) => [esc(r.username), String(r.id), r.hero ? esc(`${r.hero}, renown ${r.renown ?? 0}`) : 'NO_DESCRIPTION']);
  }

  /** Per row: activity index, score, player name, clan name. The game sorts each activity itself. */
  private board(clanParam: string): string[] {
    const c = this.findClan(clanParam);
    if (!c) return [];
    const out: string[] = [];
    for (let act = 0; act < ACTIVITY_COUNT; act++) {
      const rows = this.db.all<{ username: string; score: number }>(
        `SELECT a.username, s.score FROM scoreboard_scores s
         JOIN clan_members m ON m.account_id = s.account_id AND m.clan_id = ?
         JOIN accounts a ON a.id = s.account_id
         WHERE s.activity = ?
         ORDER BY CASE WHEN s.low_is_best = 1 THEN s.score ELSE -s.score END, s.updated_at
         LIMIT ${ROWS_PER_ACTIVITY}`,
        c.id,
        act,
      );
      for (const r of rows) out.push(String(act), fmtScore(r.score), esc(r.username), esc(c.name));
    }
    return out;
  }

  /** Triples of activity, "LIB" (low is best) or "HIB", score. Keeps each player's best. */
  private upload(accountId: number, params: string[], now: number): void {
    this.db.tx(() => {
      for (let i = 0; i + 2 < params.length && i < 3 * ACTIVITY_COUNT; i += 3) {
        const act = Number.parseInt(params[i]!, 10);
        const lowIsBest = params[i + 1] === 'LIB';
        const score = Number.parseFloat(params[i + 2]!);
        if (!Number.isInteger(act) || act < 0 || act >= ACTIVITY_COUNT) continue;
        if (!Number.isFinite(score) || score < 0 || score > 1e9) continue;
        const cur = this.db.get<{ score: number }>('SELECT score FROM scoreboard_scores WHERE account_id = ? AND activity = ?', accountId, act);
        if (cur && (lowIsBest ? score >= cur.score : score <= cur.score)) continue;
        this.db.run(
          `INSERT INTO scoreboard_scores (account_id, activity, score, low_is_best, updated_at) VALUES (?, ?, ?, ?, ?)
           ON CONFLICT(account_id, activity) DO UPDATE SET score = excluded.score, low_is_best = excluded.low_is_best, updated_at = excluded.updated_at`,
          accountId,
          act,
          score,
          lowIsBest ? 1 : 0,
          now,
        );
      }
    });
  }
}

function fmtScore(n: number): string {
  return Number.isInteger(n) ? String(n) : n.toFixed(3).replace(/\.?0+$/, '');
}
