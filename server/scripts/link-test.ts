/**
 * Runs the mod's own connection code (client-mod/tests/link_test.cpp) against this server:
 * join with a launcher ticket, walk, get a refused jump corrected, drop and resume.
 *
 *   npm run link-test -- <path to link_test.exe>          (Windows)
 *   npm run link-test -- <path to link_test.exe> wine     (Linux, with wine)
 *
 * Build link_test.exe with the client mod: cmake ... -DFO_BUILD_TESTS=ON.
 */
import { spawn } from 'node:child_process';
import type { AddressInfo } from 'node:net';
import { createApp } from '../src/app.js';
import { MemorySink } from '../src/analytics/events.js';
import { listen } from '../src/net/server.js';

const exe = process.argv[2];
const runner = process.argv[3];
if (!exe) {
  console.log('Usage: npm run link-test -- <path to link_test.exe> [wine]');
  process.exit(2);
}
const sink = new MemorySink();
const app = createApp(
  { dbPath: ':memory:', eventsPath: '', regionsPath: '/nonexistent', gameDataPath: '', spawnsPath: '', questsPath: '', contentDir: '', thingsPath: '', townDataPath: '', newHeroPhase: 'world', tickRate: 20 },
  { events: sink },
);
const server = await listen(app, 0);
const port = (server.address() as AddressInfo).port;
app.director.start();
const acct = app.accounts.register('link_test', 'password123');
const hero = app.heroes.create(acct.id, 'Linker');
const ticket = app.accounts.issueJoinTicket(acct.id, hero.id, app.cfg.realmId);

const args = ['127.0.0.1', String(port), ticket];
const child = runner ? spawn(runner, [exe, ...args], { stdio: 'inherit' }) : spawn(exe, args, { stdio: 'inherit' });
const code: number = await new Promise((r) => child.on('exit', (c) => r(c ?? 1)));
const seen = (type: string) => sink.events.filter((e: any) => e.type === type).length;
console.log(`server saw: ${seen('move_anchor')} anchor, ${seen('move_rejected')} refused runs, ${seen('player_resumed')} resumed`);
server.close();
app.close();
process.exit(code);
