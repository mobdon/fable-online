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
