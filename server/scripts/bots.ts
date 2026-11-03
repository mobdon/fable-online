/**
 * Network load test: N bots join over real WebSockets and walk around sending input at 20 Hz.
 *   npm run bots -- 1000 30      (bots, seconds)
 * Runs the server in-process with an in-memory DB so it needs no setup.
 */
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { createApp } from '../src/app.js';
import { NullSink } from '../src/analytics/events.js';
import { listen } from '../src/net/server.js';

const N = Number(process.argv[2] ?? 1000);
const SECONDS = Number(process.argv[3] ?? 20);
const REGIONS = ['HeroGuildComplex', 'LookoutPoint', 'BowerstoneSlums', 'Greatwood', 'ArenaExterior', 'PicnicArea', 'GuildWoods', 'OakValeWest'];

const app = createApp({ dbPath: ':memory:', eventsPath: '', maxChannels: 64 }, { events: new NullSink() });
const server = await listen(app, 0);
const port = (server.address() as AddressInfo).port;
app.director.start();

console.log(`creating ${N} heroes...`);
const tickets: string[] = [];
app.db.tx(() => {
  for (let i = 0; i < N; i++) {
    const { lastInsertRowid: acct } = app.db.run("INSERT INTO accounts (username, password_hash, created_at) VALUES (?, 'x', 0)", `bot${i}`);
    const name = 'Bot' + i.toString(26).replace(/./g, (c) => String.fromCharCode(97 + parseInt(c, 26)));
    const h = app.heroes.create(acct, name);
    app.db.run("UPDATE heroes SET phase = 'world', region = ? WHERE id = ?", REGIONS[i % REGIONS.length]!, h.id);
    tickets.push(app.accounts.issueJoinTicket(acct, h.id, app.cfg.realmId));
  }
});

let ticks = 0;
let bytes = 0;
let errors = 0;
const sockets: WebSocket[] = [];
console.log(`connecting ${N} bots...`);
for (let i = 0; i < N; i++) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/play`);
  sockets.push(ws);
  let seq = 0;
  let timer: NodeJS.Timeout;
  ws.on('open', () => {
    ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'bot', token: tickets[i] }));
    timer = setInterval(() => {
      if (ws.readyState === ws.OPEN)
        ws.send(JSON.stringify({ t: 'input', seq: ++seq, move_x: Math.random() * 2 - 1, move_y: 1, buttons: 0, camera_yaw: seq / 20 }));
    }, 50);
  });
  ws.on('message', (d) => {
    bytes += (d as Buffer).length;
    if ((d as Buffer)[6] === 116 /* "t" of "tick" */) ticks++;
    else if (d.toString().includes('"error"')) errors++;
  });
  ws.on('close', () => clearInterval(timer));
  if (i % 100 === 99) await new Promise((r) => setTimeout(r, 50));
}

const worst: number[] = [];
const sample = setInterval(() => worst.push(app.director.lastTickMs), 100);
await new Promise((r) => setTimeout(r, 2000));
ticks = 0;
bytes = 0;
const t0 = Date.now();
await new Promise((r) => setTimeout(r, SECONDS * 1000));
const secs = (Date.now() - t0) / 1000;
clearInterval(sample);

const s = app.director.stats();
worst.sort((a, b) => a - b);
console.log(`
players online      ${s.players} / ${N}   (join errors: ${errors})
shared channels     ${s.sharedChannels} across ${s.regions.length} regions
ticks per bot/sec   ${(ticks / N / secs).toFixed(1)}  (target ${app.cfg.tickRate})
outbound            ${((bytes * 8) / secs / 1e6).toFixed(1)} Mbit/s total, ${((bytes / N / secs) / 1024).toFixed(1)} KB/s per player (JSON, uncompressed)
server tick         p50 ${worst[Math.floor(worst.length / 2)]?.toFixed(2)} ms, p99 ${worst[Math.floor(worst.length * 0.99)]?.toFixed(2)} ms (budget ${(1000 / app.cfg.tickRate).toFixed(0)} ms)
`);
for (const ws of sockets) ws.terminate();
server.close();
app.close();
process.exit(0);
