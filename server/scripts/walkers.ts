/**
 * Stand-in players for a one-PC test: bots that log in to a running server like real
 * players (launcher API, join ticket, game socket) and walk around a hero, sending the same
 * position reports the mod sends. In game you see them as stand-in heroes with name tags.
 *
 *   npm run walkers -- --near Mob                3 walkers around the hero called Mob
 *   npm run walkers -- --near Mob --count 5 --server http://127.0.0.1:7777
 *
 * The server must be on the same machine (the walkers put their heroes in the target's region
 * through the database, FO_DB) and should run with FO_NEW_HERO_PHASE=world, as
 * ops/local-host/start-server.bat does. Walkers keep their accounts between runs.
 */
import { WebSocket } from 'ws';
import { Database } from '../src/db/database.js';

const args = new Map<string, string>();
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i]!;
  if (a.startsWith('--')) args.set(a.slice(2), process.argv[i + 1] ?? '');
}
const target = args.get('near') ?? '';
const count = Math.max(1, Math.min(20, Number(args.get('count') ?? 3)));
const base = (args.get('server') ?? 'http://127.0.0.1:7777').replace(/\/+$/, '');
const NAMES = ['Wanderer', 'Rambler', 'Strider', 'Roamer', 'Drifter', 'Pilgrim', 'Nomad', 'Ranger', 'Tramper', 'Hiker'];
const STEP_HZ = 10;
/** Metres per report: 7 m/s at 10 Hz, inside the server's checks. */
const MAX_STEP = 0.7;

if (!target) {
  console.log('Usage: npm run walkers -- --near <YourHeroName> [--count 3] [--server http://127.0.0.1:7777]');
  process.exit(1);
}

const db = new Database(process.env.FO_DB || 'data/fable-online.sqlite');
const home = db.get<{ id: number; region: string; x: number; y: number; z: number; phase: string }>(
  'SELECT id, region, x, y, z, phase FROM heroes WHERE name = ?',
  target,
);
if (!home) {
  console.error(`No hero called ${target} on this server yet. Log in once through the launcher first.`);
  process.exit(1);
}
if (home.phase !== 'world') console.warn(`${target} is still in ${home.phase}, a private instance: walkers cannot join it there.`);

async function api(path: string, body: object, token?: string): Promise<any> {
  const res = await fetch(base + path, {
    method: 'POST',
    headers: { 'content-type': 'application/json', ...(token ? { authorization: `Bearer ${token}` } : {}) },
    body: JSON.stringify(body),
  });
  const json = (await res.json().catch(() => ({}))) as any;
  return { ok: res.ok, ...json };
}

async function login(name: string): Promise<{ token: string; heroId: number }> {
  const user = `walker_${name.toLowerCase()}`;
  const pw = 'walker-password-1';
  let auth = await api('/launcher/v1/login', { username: user, password: pw });
  if (!auth.access_token) auth = await api('/launcher/v1/register', { username: user, password: pw });
  if (!auth.access_token) throw new Error(`${name}: cannot log in or register (${auth.error ?? auth.message ?? 'no reason'})`);
  const token: string = auth.access_token;
  let hero = db.get<{ id: number }>('SELECT id FROM heroes WHERE name = ?', name);
  if (!hero) {
    const made = await api('/launcher/v1/heroes', { name }, token);
    if (!made.hero) throw new Error(`${name}: cannot make a hero (${made.error ?? made.message})`);
    hero = { id: made.hero.id };
  }
  // Same region as the target, at the origin so the first report anchors next to them.
  db.run("UPDATE heroes SET phase = 'world', region = ?, x = 0, y = 0, z = 0 WHERE id = ?", home!.region, hero.id);
  return { token, heroId: hero.id };
}

function walk(name: string, index: number, ticket: string, host: string, port: number, heroId: number) {
  const ws = new WebSocket(`ws://${host}:${port}/play`);
  let me = { x: home!.x, y: home!.y, z: home!.z };
  let centre = { ...me };
  let anchored = false;
  let seenAt = 0;
  let readAt = 0;
  let angle = (index / count) * Math.PI * 2;
  const radius = 3 + index * 1.5;
  let timer: NodeJS.Timeout | undefined;

  ws.on('open', () => ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'walker', token: ticket })));
  ws.on('message', (d) => {
    const m = JSON.parse(d.toString());
    if (m.t === 'welcome') {
      console.log(`${name}: in the world (${m.client_move} position reports)`);
      timer = setInterval(step, 1000 / STEP_HZ);
    } else if (m.t === 'tick') {
      const t = m.entities.find((e: any) => e.kind === 'hero' && e.name === target);
      if (t) {
        centre = { x: t.x, y: t.y, z: t.z };
        seenAt = Date.now();
      }
    } else if (m.t === 'pos_correct') {
      me = { x: m.x, y: m.y, z: m.z };
    } else if (m.t === 'enter_region' && m.region !== home!.region) {
      console.log(`${name}: moved to ${m.region}`);
    } else if (m.t === 'error') {
      console.log(`${name}: server said ${m.error}: ${m.message}`);
    }
  });
  ws.on('close', (code) => {
    clearInterval(timer);
    console.log(`${name}: disconnected (${code})`);
  });

  function step() {
    // Out of sight (more than the server's interest radius away): find the target through the
    // database, which the server writes when a hero anchors and every persist after that.
    const now = Date.now();
    if (now - seenAt > 2000 && now - readAt > 2000) {
      readAt = now;
      const p = db.get<{ region: string; x: number; y: number; z: number }>('SELECT region, x, y, z FROM heroes WHERE id = ?', home!.id);
      if (p && p.region === home!.region && (p.x || p.y || p.z)) centre = { x: p.x, y: p.y, z: p.z };
    }
    // Hold the first report (it anchors us) until we know where the target is.
    if (!anchored && !centre.x && !centre.y && !centre.z) return;
    angle += 0.25 / radius + 0.02;
    const want = { x: centre.x + radius * Math.cos(angle), y: centre.y + radius * Math.sin(angle), z: centre.z };
    if (!anchored) {
      me = want;
      anchored = true;
    } else {
      const dx = want.x - me.x;
      const dy = want.y - me.y;
      const d = Math.hypot(dx, dy);
      const k = d > MAX_STEP ? MAX_STEP / d : 1;
      me = { x: me.x + dx * k, y: me.y + dy * k, z: want.z };
    }
    const yaw = Math.atan2(-(want.x - me.x), want.y - me.y);
    ws.send(JSON.stringify({ t: 'hero_pos', x: round(me.x), y: round(me.y), z: round(me.z), yaw: round(yaw), area: 0 }));
  }
  void heroId;
  return ws;
}

const round = (n: number) => Math.round(n * 100) / 100;

console.log(`${count} walkers joining ${target} in ${home.region} via ${base}`);
const sockets: WebSocket[] = [];
for (let i = 0; i < count; i++) {
  const name = NAMES[i % NAMES.length]! + (i >= NAMES.length ? String.fromCharCode(97 + Math.floor(i / NAMES.length)) : '');
  try {
    const { token, heroId } = await login(name);
    const join = await api('/launcher/v1/join', { region: process.env.FO_REALM_ID || 'albion-1', client_version: '0.1.0', hero_id: heroId }, token);
    if (!join.ticket) throw new Error(`${name}: no join ticket (${join.error ?? join.message})`);
    // Connect to the address we were given, not the public one the server hands out: a home
    // router often cannot loop back to its own public address.
    sockets.push(walk(name, i, join.ticket, new URL(base).hostname, join.port, heroId));
  } catch (e) {
    console.error((e as Error).message);
  }
}
process.on('SIGINT', () => {
  for (const ws of sockets) ws.close();
  setTimeout(() => process.exit(0), 200);
});
