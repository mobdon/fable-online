import fs from 'node:fs';
import path from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';
import type { App } from '../app.js';
import { GameError } from '../errors.js';

const BODY_LIMIT = 8 * 1024;
const LOGO_BODY_LIMIT = 768 * 1024;
const SCOREBOARD_BODY_LIMIT = 64 * 1024;
const NO_CONTENT = Symbol('204');

type Handler = (req: IncomingMessage, body: Record<string, unknown>, url: URL) => unknown;

/**
 * HTTP API. /launcher/v1/* is the contract the launcher speaks (shared/protocol/launcher-api.md).
 * /admin/v1/* needs an admin account.
 */
export function createHttpHandler(app: App) {
  const { cfg, accounts, heroes, director, economy, scoreboard } = app;
  const bearer = (req: IncomingMessage) => req.headers.authorization?.replace(/^Bearer\s+/i, '');
  const routes = new Map<string, Handler>();
  const route = (method: string, p: string, h: Handler) => routes.set(`${method} ${p}`, h);

  route('GET', '/launcher/v1/status', () => {
    const online = director.online;
    const capacity = director.capacity;
    return {
      online: true,
      motd: cfg.motd,
      protocol: cfg.protocolVersion,
      server_time: new Date().toISOString(),
      regions: [
        {
          id: cfg.realmId,
          name: cfg.realmName,
          status: online >= capacity ? 'full' : online >= capacity * 0.8 ? 'busy' : 'online',
          players: online,
          capacity,
          ping_host: cfg.publicHost,
          ping_port: cfg.publicPort,
        },
      ],
    };
  });

  route('GET', '/launcher/v1/news', () => readContent(cfg.contentDir, 'news.json', { items: [] }));

  route('GET', '/launcher/v1/manifest', (_req, _b, url) => {
    const channel = url.searchParams.get('channel') ?? 'live';
    if (!/^[a-z0-9-]{1,32}$/.test(channel)) throw new GameError('bad_channel', 'Bad channel');
    const m = readContent(cfg.contentDir, `manifest-${channel}.json`, null);
    // No manifest published: nothing to patch. The launcher still has to see one to let the
    // player in, and on a home test server everyone brings their own build.
    return m ?? { version: cfg.minClientVersion, protocol: cfg.protocolVersion, files: [] };
  });

  route('POST', '/launcher/v1/register', (req, b) =>
    accounts.registerAndLogin(String(b.username ?? ''), String(b.password ?? ''), req.socket.remoteAddress),
  );

  route('POST', '/launcher/v1/login', (req, b) => accounts.login(String(b.username ?? ''), String(b.password ?? ''), req.socket.remoteAddress));
  route('POST', '/launcher/v1/refresh', (_req, b) => accounts.refresh(String(b.refresh_token ?? '')));
  route('POST', '/launcher/v1/logout', (req) => {
    accounts.logout(bearer(req) ?? '');
    return NO_CONTENT;
  });

  route('GET', '/launcher/v1/heroes', (req) => {
    const acct = accounts.authenticate(bearer(req));
    return { heroes: heroes.listForAccount(acct.id).map(publicHero) };
  });
  route('POST', '/launcher/v1/heroes', (req, b) => {
    const acct = accounts.authenticate(bearer(req));
    return { hero: publicHero(heroes.create(acct.id, String(b.name ?? ''))) };
  });

  route('POST', '/launcher/v1/join', (req, b) => {
    const acct = accounts.authenticate(bearer(req));
    if (b.region !== undefined && b.region !== cfg.realmId) throw new GameError('bad_region', `Unknown region ${String(b.region)}`, 404);
    if (versionLess(String(b.client_version ?? '0'), cfg.minClientVersion))
      throw new GameError('outdated', `Please update: client ${cfg.minClientVersion} or newer is required`, 409);
    const wanted = b.hero_id === undefined ? undefined : Number(b.hero_id);
    const hero =
      wanted === undefined && cfg.autoHero && heroes.listForAccount(acct.id).length === 0
        ? heroes.create(acct.id, heroNameFor(acct.username, (name) => !!app.db.get('SELECT 1 FROM heroes WHERE name = ?', name)))
        : heroes.pick(acct.id, wanted);
    if (director.online >= director.capacity) throw new GameError('region_full', 'The server is full', 503);
    return {
      ticket: accounts.issueJoinTicket(acct.id, hero.id, cfg.realmId),
      host: cfg.publicHost,
      port: cfg.publicPort,
      hero: publicHero(hero),
      protocol: cfg.protocolVersion,
      // Realm chat: the mod logs in with this token; there is no login by name.
      ...(app.chat ? { chat: app.chat.joinInfo(acct, hero) } : {}),
    };
  });

  const admin = (req: IncomingMessage) => {
    const acct = accounts.authenticate(bearer(req));
    if (acct.role !== 'admin') throw new GameError('forbidden', 'Admins only', 403);
  };
  // Fable's own scoreboard login: the player types their account name and this code.
  route('POST', '/launcher/v1/scoreboard-code', (req) => scoreboard.rotateCode(accounts.authenticate(bearer(req)).id));

  const clanArg = (b: Record<string, unknown>) => {
    const c = typeof b.clan_id === 'number' ? scoreboard.clan(b.clan_id) : scoreboard.clanByName(String(b.clan ?? ''));
    if (!c) throw new GameError('no_clan', 'No such clan', 404);
    return c;
  };
  const accountArg = (b: Record<string, unknown>) => {
    const a = app.db.get<{ id: number }>('SELECT id FROM accounts WHERE username = ?', String(b.username ?? ''));
    if (!a) throw new GameError('no_account', 'No such account', 404);
    return a.id;
  };
  route('POST', '/admin/v1/clans', (req, b) => {
    admin(req);
    const c = scoreboard.createClan(String(b.name ?? ''), String(b.description ?? ''));
    return { id: c.id, name: c.name };
  });
  route('POST', '/admin/v1/clans/members', (req, b) => {
    admin(req);
    scoreboard.addMember(clanArg(b).id, accountArg(b));
    return NO_CONTENT;
  });
  route('POST', '/admin/v1/clans/members/remove', (req, b) => {
    admin(req);
    scoreboard.removeMember(clanArg(b).id, accountArg(b));
    return NO_CONTENT;
  });
  route('POST', '/admin/v1/clans/logo', (req, b) => {
    admin(req);
    return scoreboard.setLogo(clanArg(b).id, Buffer.from(String(b.jpeg_base64 ?? ''), 'base64'));
  });
  route('GET', '/admin/v1/scoreboard/recent', (req) => (admin(req), { requests: scoreboard.recent }));

  route('GET', '/admin/v1/stats', (req) => (admin(req), director.stats()));
  route('GET', '/admin/v1/economy/audit', (req) => (admin(req), economy.audit()));

  return async (req: IncomingMessage, res: ServerResponse) => {
    const url = new URL(req.url ?? '/', 'http://x');
    // Fable.exe's scoreboard client (binary body, not JSON).
    if (req.method === 'POST' && url.pathname === '/index.php') {
      try {
        const raw = await readRaw(req, SCOREBOARD_BODY_LIMIT);
        const out = scoreboard.handle(req.headers, raw);
        res.writeHead(out.status, { 'content-type': 'application/octet-stream', 'content-length': out.body.length, connection: 'close' });
        res.end(out.body);
      } catch {
        res.writeHead(413, { connection: 'close' });
        res.end();
      }
      return;
    }
    const h = routes.get(`${req.method} ${url.pathname}`);
    try {
      if (!h) throw new GameError('not_found', 'Not found', 404);
      const body = req.method === 'POST' ? await readJson(req, url.pathname === '/admin/v1/clans/logo' ? LOGO_BODY_LIMIT : BODY_LIMIT) : {};
      const out = await h(req, body, url);
      if (out === NO_CONTENT) {
        res.writeHead(204);
        res.end();
      } else send(res, 200, out);
    } catch (err) {
      if (err instanceof GameError) send(res, err.status, { error: err.code, message: err.message });
      else {
        console.error(err);
        send(res, 500, { error: 'internal', message: 'Something went wrong' });
      }
    }
  };
}

export function publicHero(h: { id: number; name: string; phase: string; renown: number; region: string; pvp_unlocked: number }) {
  return { id: h.id, name: h.name, phase: h.phase, renown: h.renown, region: h.region, pvp_unlocked: !!h.pvp_unlocked };
}

function send(res: ServerResponse, status: number, body: unknown) {
  const s = JSON.stringify(body);
  res.writeHead(status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(s), 'cache-control': 'no-store' });
  res.end(s);
}

function readRaw(req: IncomingMessage, limit: number): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks: Buffer[] = [];
    req.on('data', (c: Buffer) => {
      size += c.length;
      if (size > limit) {
        reject(new GameError('too_large', 'Body too large', 413));
        req.destroy();
      } else chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function readJson(req: IncomingMessage, limit = BODY_LIMIT): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks: Buffer[] = [];
    req.on('data', (c: Buffer) => {
      size += c.length;
      if (size > limit) {
        reject(new GameError('too_large', 'Body too large', 413));
        req.destroy();
      } else chunks.push(c);
    });
    req.on('end', () => {
      if (chunks.length === 0) return resolve({});
      try {
        const v = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        resolve(v && typeof v === 'object' && !Array.isArray(v) ? v : {});
      } catch {
        reject(new GameError('bad_json', 'Body must be JSON'));
      }
    });
    req.on('error', reject);
  });
}

function readContent<T>(dir: string, file: string, fallback: T): unknown {
  try {
    return JSON.parse(fs.readFileSync(path.join(dir, file), 'utf8'));
  } catch {
    return fallback;
  }
}

/** "0.2.10" < "0.10.0" compares numerically per part. */
export function versionLess(a: string, b: string): boolean {
  const pa = a.split('.').map((n) => parseInt(n, 10) || 0);
  const pb = b.split('.').map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d < 0;
  }
  return false;
}

/**
 * "mob_99" -> "Mob": the letters of a username, as a Fable-style hero name. Hero names are
 * letters only, so when the name is taken ("mob" and "mob_99") a letter suffix tells them
 * apart: "Moba", "Mobb", ... then two letters.
 */
export function heroNameFor(username: string, taken: (name: string) => boolean = () => false): string {
  const letters = username.replace(/[^A-Za-z]/g, '').slice(0, 16).toLowerCase();
  const name = letters.charAt(0).toUpperCase() + letters.slice(1);
  if (name.length < 3) throw new GameError('no_hero', 'Make a hero first: the account name has too few letters to name one after', 404);
  if (!taken(name)) return name;
  const letter = (i: number) => String.fromCharCode(97 + i);
  for (let n = 0; n < 26 + 26 * 26; n++) {
    const suffix = n < 26 ? letter(n) : letter(Math.floor(n / 26) - 1) + letter(n % 26);
    const candidate = name.slice(0, 16 - suffix.length) + suffix;
    if (!taken(candidate)) return candidate;
  }
  throw new GameError('name_taken', 'Make a hero first: no free name was found for this account', 409);
}
