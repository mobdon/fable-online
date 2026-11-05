import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { testApp, makeHero, link, key } from './helpers.js';
import { listen } from '../src/net/server.js';
import { signToken } from '../src/chat/auth.js';
import { chatRole, regionKey } from '../src/chat/realm-chat.js';
import type { Conn } from '../src/chat/hub.js';
import type { ServerMsg } from '../src/chat/protocol.js';

function conn() {
  const inbox: ServerMsg[] = [];
  let closed: string | null = null;
  const c: Conn = { send: (m) => void inbox.push(m), close: (r) => void (closed = r) };
  return { c, inbox, closed: () => closed };
}

test('/join hands out a chat token that the in-server hub accepts', async () => {
  const app = testApp({ chatPublicHost: 'chat.example', chatPublicPort: 7781 });
  const server = await listen(app, 0);
  const base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  try {
    const post = async (path: string, body: unknown, token?: string) =>
      (await fetch(base + path, {
        method: 'POST',
        headers: { 'content-type': 'application/json', ...(token ? { authorization: `Bearer ${token}` } : {}) },
        body: JSON.stringify(body),
      })).json() as Promise<any>;
    const reg = await post('/launcher/v1/register', { username: 'Whisper', password: 'password123' });
    const hero = (await post('/launcher/v1/heroes', { name: 'Whisper' }, reg.access_token)).hero;
    const join = await post('/launcher/v1/join', { client_version: '0.1.0' }, reg.access_token);
    assert.equal(join.chat.host, 'chat.example');
    assert.equal(join.chat.port, 7781);
    assert.ok(join.chat.token);

    const ok = conn();
    app.chat!.hub.open(ok.c);
    app.chat!.hub.handle(ok.c, { t: 'hello', v: 1, token: join.chat.token });
    const welcome = ok.inbox.find((m) => m.t === 'welcome');
    assert.ok(welcome && welcome.t === 'welcome');
    assert.equal(welcome.name, hero.name);
    assert.equal(app.chat!.hub.online, 1);
  } finally {
    server.close();
    app.close();
  }
});

test('chat refuses login by name and tokens it did not sign', () => {
  const app = testApp();
  try {
    const byName = conn();
    app.chat!.hub.open(byName.c);
    app.chat!.hub.handle(byName.c, { t: 'hello', v: 1, name: 'Sneaky' });
    assert.equal(byName.closed(), 'auth');

    const forged = conn();
    app.chat!.hub.open(forged.c);
    const token = signToken({ sub: '1', name: 'Forger', role: 'admin', exp: Date.now() / 1000 + 60 }, 'not-the-secret');
    app.chat!.hub.handle(forged.c, { t: 'hello', v: 1, token });
    assert.equal(forged.closed(), 'auth');
    assert.equal(app.chat!.hub.online, 0);
  } finally {
    app.close();
  }
});

test('expired chat tokens are refused', () => {
  const app = testApp({ chatTokenTtlMs: 1000 });
  try {
    const { acct, hero } = makeHero(app);
    const old = app.chat!.token(acct, hero, Date.now() - 5000);
    const c = conn();
    app.chat!.hub.open(c.c);
    app.chat!.hub.handle(c.c, { t: 'hello', v: 1, token: old });
    assert.equal(c.closed(), 'auth');
  } finally {
    app.close();
  }
});

test('positions, regions and name tags come from the world, not the client', () => {
  const app = testApp();
  try {
    const a = makeHero(app);
    const b = makeHero(app);
    const ch = app.director.join(app.heroes.get(a.hero.id)!, link(a.hero.id, a.hero.name, []));
    app.director.join(app.heroes.get(b.hero.id)!, link(b.hero.id, b.hero.name, []));
    const ca = conn(), cb = conn();
    for (const [c, h] of [[ca, a], [cb, b]] as const) {
      app.chat!.hub.open(c.c);
      app.chat!.hub.handle(c.c, { t: 'hello', v: 1, token: app.chat!.token(h.acct, h.hero) });
    }
    // A client position report is ignored: the server owns positions.
    app.chat!.hub.handle(ca.c, { t: 'pos', region: 99, x: 5000, y: 0, z: 0 });
    app.chat!.sync([a.hero.id, b.hero.id], Date.now() + 1000);
    const tags = ca.inbox.find((m) => m.t === 'tags');
    assert.ok(tags && tags.t === 'tags', 'name tags are sent');
    assert.equal(tags.region, regionKey(ch.id));
    assert.deepEqual(tags.add.map((t) => t.name), [b.hero.name]);

    app.chat!.hub.handle(cb.c, { t: 'say', ch: 'region', text: 'hello region' });
    assert.ok(ca.inbox.some((m) => m.t === 'msg' && m.text === 'hello region'));
  } finally {
    app.close();
  }
});

test('account roles map to chat roles', () => {
  assert.equal(chatRole('player'), 'player');
  assert.equal(chatRole('gm'), 'mod');
  assert.equal(chatRole('admin'), 'admin');
});

test('server events reach the analytics metrics', async () => {
  const app = testApp();
  try {
    const { acct, hero } = makeHero(app);
    app.accounts.login(acct.username, 'password123');
    assert.throws(() => app.accounts.login(acct.username, 'wrong-password'));
    app.economy.grant(hero.id, 250, 'quest_reward', key());
    app.economy.spend(hero.id, 100, 'repair', key());
    app.director.join(app.heroes.get(hero.id)!, link(hero.id, hero.name, []));
    const c = conn();
    app.chat!.hub.open(c.c);
    app.chat!.hub.handle(c.c, { t: 'hello', v: 1, token: app.chat!.token(acct, hero) });
    app.chat!.hub.handle(c.c, { t: 'say', ch: 'realm', text: 'hi all' });

    const text = await app.analytics.metrics.registry.metrics();
    assert.match(text, /fable_logins_total\{result="ok"\} 1/);
    assert.match(text, /fable_logins_total\{result="bad_password"\} 1/);
    assert.match(text, /fable_gold_minted_total\{source="quest_reward",region="unknown"\} 250/);
    assert.match(text, /fable_gold_burned_total\{sink="repair",region="unknown"\} 100/);
    assert.match(text, /fable_players_online\{region="[^"]+"\} 1/);
    assert.match(text, /fable_chat_messages_total\{channel="realm"\} 1/);
    assert.equal(app.analytics.goldSupply, 150);
    assert.ok(app.sink.events.some((e) => e.type === 'chat' && e.event === 'chat.message'), 'chat events reach the game event log');
  } finally {
    app.close();
  }
});
