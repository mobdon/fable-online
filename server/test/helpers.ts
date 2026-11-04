import { createApp, type App } from '../src/app.js';
import { MemorySink } from '../src/analytics/events.js';
import type { ServerConfig } from '../src/config.js';

export function testApp(overrides: Partial<ServerConfig> = {}): App & { sink: MemorySink } {
  const sink = new MemorySink();
  const app = createApp({ dbPath: ':memory:', eventsPath: '', regionsPath: '/nonexistent', gameDataPath: '', spawnsPath: '', questsPath: '', contentDir: '', thingsPath: '', townDataPath: '', ...overrides }, { events: sink });
  return Object.assign(app, { sink });
}

let n = 0;
export function makeHero(app: App, phase: 'childhood' | 'guild_training' | 'world' = 'world') {
  n++;
  const acct = app.accounts.register(`user_${n}_${Math.random().toString(36).slice(2, 8)}`, 'password123');
  const name = 'Hero' + String.fromCharCode(97 + (n % 26)) + n.toString(36).replace(/[0-9]/g, (d) => String.fromCharCode(103 + Number(d)));
  let hero = app.heroes.create(acct.id, name);
  if (phase !== 'childhood') hero = app.heroes.advancePhase(hero.id, 'guild_training');
  if (phase === 'world') hero = app.heroes.advancePhase(hero.id, 'world');
  return { acct, hero };
}

export const key = () => `k-${Math.random().toString(36).slice(2)}-${Date.now()}`;

/** A player link that collects encoded frames, for tests that drive channels directly. */
export function link(heroId: number, name: string, inbox: string[]): import('../src/world/channel.js').PlayerLink {
  return { heroId, name, renown: 500, lastSeq: -1, joinedAt: Date.now(), send: (f) => inbox.push(f) };
}

/** A game socket client for e2e tests: `next` waits for the first frame matching `pred`. */
export async function wsClient(url: string) {
  const { WebSocket } = await import('ws');
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  const waiters: [(m: any) => boolean, (m: any) => void][] = [];
  ws.on('message', (d) => {
    const m = JSON.parse(d.toString());
    inbox.push(m);
    for (const w of [...waiters])
      if (w[0](m)) {
        waiters.splice(waiters.indexOf(w), 1);
        w[1](m);
      }
  });
  await new Promise<void>((r) => ws.on('open', () => r()));
  const next = (pred: (m: any) => boolean) =>
    new Promise<any>((resolve, reject) => {
      const hit = inbox.find(pred);
      if (hit) return resolve(hit);
      waiters.push([pred, resolve]);
      setTimeout(() => reject(new Error('timed out waiting for a frame')), 3000);
    });
  return { ws, next, inbox, send: (m: Record<string, unknown>) => ws.send(JSON.stringify(m)) };
}

/** Registers an account, makes a graduated hero and gets a join ticket over HTTP. */
export async function joinAs(app: App, base: string, name: string) {
  const post = (p: string, body: unknown, tok?: string) =>
    fetch(base + p, {
      method: 'POST',
      headers: { 'content-type': 'application/json', ...(tok ? { authorization: `Bearer ${tok}` } : {}) },
      body: JSON.stringify(body),
    }).then((r) => r.json() as Promise<any>);
  const tok = (await post('/launcher/v1/register', { username: name, password: 'password123' })).access_token;
  const hero = (await post('/launcher/v1/heroes', { name }, tok)).hero;
  app.heroes.advancePhase(hero.id, 'guild_training');
  app.heroes.advancePhase(hero.id, 'world');
  const ticket = (await post('/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, tok)).ticket;
  return { hero, ticket };
}
