// Dev-only stand-in for the real gateway, so the launcher can be tested
// before server/ exists. Implements shared/protocol/launcher-api.md in memory.
// Not for players: accounts vanish on restart.
//
//   node tools/dev-gateway/gateway.mjs [--port 8787] [--files <folder>] [--version 0.1.0]
//
// --files serves every file in that folder as the patch manifest (point it at
// the folder holding fable_online.dll and fable_online_launcher.exe).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) =>
  a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc, []));
const port = Number(args.port || 8787);
const filesDir = args.files ? path.resolve(args.files) : null;
const version = args.version || '0.1.0';

const users = new Map();     // name -> { id, salt, hash }
const access = new Map();    // token -> { name, exp }
const refresh = new Map();   // token -> name
const tickets = new Map();   // ticket -> { name, region, exp }
const regions = [
  { id: 'albion-1', name: 'Albion 1', status: 'online', players: 42, capacity: 1000, ping_host: '127.0.0.1', ping_port: 27015 },
  { id: 'albion-2', name: 'Albion 2', status: 'busy', players: 910, capacity: 1000, ping_host: '127.0.0.1', ping_port: 27016 },
  { id: 'test', name: 'Test realm', status: 'maintenance', players: 0, capacity: 200, ping_host: '127.0.0.1', ping_port: 27017 },
];
const news = [
  { id: 'n2', title: 'Dev gateway running', date: new Date().toISOString().slice(0, 10), body: 'You are talking to tools/dev-gateway.\nAccounts reset when it restarts.', url: '' },
  { id: 'n1', title: 'Welcome to Fable Online', date: '2026-11-03', body: 'The launcher works: log in, pick a region, press Play.', url: '' },
];

const token = () => crypto.randomBytes(24).toString('base64url');
const hashPw = (pw, salt) => crypto.scryptSync(pw, salt, 32).toString('hex');

function issue(name) {
  const a = token(), r = token();
  access.set(a, { name, exp: Date.now() + 900_000 });
  refresh.set(r, name);
  return { access_token: a, refresh_token: r, expires_in: 900, account: { id: users.get(name).id, name } };
}

function manifest() {
  const files = [];
  if (filesDir && fs.existsSync(filesDir)) {
    const walk = (dir) => {
      for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, e.name);
        if (e.isDirectory()) walk(full);
        else if (!e.name.endsWith('.part') && !e.name.endsWith('.old')) {
          const data = fs.readFileSync(full);
          files.push({ path: path.relative(filesDir, full).split(path.sep).join('/'), size: data.length,
                       sha256: crypto.createHash('sha256').update(data).digest('hex') });
        }
      }
    };
    walk(filesDir);
  }
  return { version, protocol: 1, base_url: `http://127.0.0.1:${port}/files/`, files };
}

function send(res, status, body) {
  const text = body === undefined ? '' : JSON.stringify(body);
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(text);
}

function bearer(req) {
  const m = /^Bearer (.+)$/.exec(req.headers.authorization || '');
  const t = m && access.get(m[1]);
  return t && t.exp > Date.now() ? t.name : null;
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  let body = {};
  if (req.method === 'POST') {
    let raw = '';
    for await (const c of req) { raw += c; if (raw.length > 65536) return send(res, 413, { error: 'too_large' }); }
    try { body = raw ? JSON.parse(raw) : {}; } catch { return send(res, 400, { error: 'bad_json' }); }
  }
  console.log(req.method, url.pathname);
  const route = `${req.method} ${url.pathname}`;

  if (url.pathname.startsWith('/files/') && req.method === 'GET' && filesDir) {
    const rel = decodeURIComponent(url.pathname.slice(7));
    const full = path.resolve(filesDir, rel);
    if (!full.startsWith(filesDir + path.sep) || !fs.existsSync(full)) return send(res, 404, { error: 'not_found' });
    const data = fs.readFileSync(full);
    res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': data.length });
    return res.end(data);
  }

  switch (route) {
    case 'GET /launcher/v1/status':
      return send(res, 200, { online: true, motd: 'Dev gateway', protocol: 1, regions });
    case 'GET /launcher/v1/news':
      return send(res, 200, { items: news });
    case 'GET /launcher/v1/manifest':
      return send(res, 200, manifest());
    case 'POST /launcher/v1/register': {
      const { username, password } = body;
      if (typeof username !== 'string' || !/^[A-Za-z0-9_]{3,16}$/.test(username))
        return send(res, 400, { error: 'bad_name', message: 'Names are 3 to 16 letters, digits or _.' });
      if (typeof password !== 'string' || password.length < 8)
        return send(res, 400, { error: 'weak_password', message: 'Use at least 8 characters.' });
      if (users.has(username.toLowerCase())) return send(res, 409, { error: 'name_taken' });
      const salt = crypto.randomBytes(16).toString('hex');
      users.set(username.toLowerCase(), { id: `a${users.size + 1}`, salt, hash: hashPw(password, salt), display: username });
      return send(res, 200, issue(username.toLowerCase()));
    }
    case 'POST /launcher/v1/login': {
      const u = users.get(String(body.username || '').toLowerCase());
      if (!u || !crypto.timingSafeEqual(Buffer.from(u.hash, 'hex'), Buffer.from(hashPw(String(body.password || ''), u.salt), 'hex')))
        return send(res, 401, { error: 'bad_credentials' });
      return send(res, 200, issue(String(body.username).toLowerCase()));
    }
    case 'POST /launcher/v1/refresh': {
      const name = refresh.get(body.refresh_token);
      if (!name) return send(res, 401, { error: 'bad_token' });
      refresh.delete(body.refresh_token);  // rotate
      return send(res, 200, issue(name));
    }
    case 'POST /launcher/v1/logout': {
      const name = bearer(req);
      if (!name) return send(res, 401, { error: 'unauthorized' });
      for (const [t, n] of refresh) if (n === name) refresh.delete(t);
      res.writeHead(204); return res.end();
    }
    case 'POST /launcher/v1/join': {
      const name = bearer(req);
      if (!name) return send(res, 401, { error: 'unauthorized' });
      const r = regions.find((x) => x.id === body.region);
      if (!r) return send(res, 404, { error: 'no_region', message: 'Unknown region.' });
      if (r.status !== 'online' && r.status !== 'busy') return send(res, 503, { error: 'region_full', message: `${r.name} is ${r.status}.` });
      if (body.client_version !== version) return send(res, 409, { error: 'outdated' });
      const t = token();
      tickets.set(t, { name, region: r.id, exp: Date.now() + 60_000 });
      return send(res, 200, { ticket: t, host: r.ping_host, port: r.ping_port });
    }
  }
  send(res, 404, { error: 'not_found' });
});

server.listen(port, '127.0.0.1', () => console.log(`dev gateway on http://127.0.0.1:${port}  files: ${filesDir || '(none)'}  version ${version}`));
