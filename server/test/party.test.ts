import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Parties, type PartyHost } from '../src/party/parties.js';
import { MemorySink } from '../src/analytics/events.js';
import { GameError } from '../src/errors.js';
import { testApp, makeHero, link, wsClient, joinAs } from './helpers.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;

/** A party system on a fake world: every hero in one shared channel unless told otherwise. */
function world(opts: { training?: number[] } = {}) {
  let t = 1_000_000;
  const where = new Map<number, { channel: string; region: string; shared: boolean }>();
  const moved: [number, string][] = [];
  const host: PartyHost = {
    whereIs: (id) => where.get(id),
    canParty: (id) => !(opts.training ?? []).includes(id),
    moveToChannel: (id, ch) => {
      moved.push([id, ch]);
      where.set(id, { ...where.get(id)!, channel: ch });
    },
  };
  const sink = new MemorySink();
  const parties = new Parties(sink, host, {}, () => t);
  const inbox = new Map<number, any[]>();
  const join = (id: number, name: string, channel = 'Bowerstone#1') => {
    where.set(id, { channel, region: 'Bowerstone', shared: true });
    inbox.set(id, []);
    parties.connect(id, name, (m) => inbox.get(id)!.push(m));
  };
  const last = (id: number, type: string) => inbox.get(id)!.filter((m) => m.t === type).at(-1);
  const notes = (id: number) => inbox.get(id)!.filter((m) => m.t === 'party_note').map((m) => m.text);
  return { parties, join, last, notes, inbox, where, moved, sink, advance: (ms: number) => (t += ms) };
}

test('an invite and an accept make a party with a leader and a colour each', () => {
  const w = world();
  w.join(1, 'Maze');
  w.join(2, 'Thunder');
  assert.deepEqual(w.last(1, 'party'), { t: 'party', id: null });
  w.parties.invite(1, 'thunder'); // names ignore case
  const inv = w.last(2, 'party_invite');
  assert.equal(inv.name, 'Maze');
  assert.equal(inv.expires_ms, 60_000);
  w.parties.accept(2);
  const p = w.last(1, 'party');
  assert.equal(p.leader, 1);
  assert.deepEqual(
    p.members.map((m: any) => [m.name, m.colour, m.leader, m.online, m.region]),
    [
      ['Maze', 'red', true, true, 'Bowerstone'],
      ['Thunder', 'blue', false, true, 'Bowerstone'],
    ],
  );
  assert.deepEqual(w.last(2, 'party'), p);
  assert.deepEqual(w.parties.companions(1), [2]);
  assert.ok(w.sink.events.some((e) => e.type === 'party_formed'));
});

test('party chat reaches members only', () => {
  const w = world();
  for (const [id, n] of [[1, 'Maze'], [2, 'Thunder'], [3, 'Whisper']] as const) w.join(id, n);
  w.parties.invite(1, 'Thunder');
  w.parties.accept(2);
  w.parties.say(2, 'to the Arena');
  assert.deepEqual(w.last(1, 'party_chat'), { t: 'party_chat', from: 2, name: 'Thunder', text: 'to the Arena' });
  assert.ok(w.last(2, 'party_chat'));
  assert.equal(w.last(3, 'party_chat'), undefined);
  assert.throws(() => w.parties.say(3, 'hello?'), code('not_in_party'));
});

test('only the leader invites, kicks and hands on the lead; a party holds four', () => {
  const w = world();
  const names = ['Maze', 'Thunder', 'Whisper', 'Briar', 'Jack'];
  names.forEach((n, i) => w.join(i + 1, n));
  w.parties.invite(1, 'Thunder');
  w.parties.accept(2);
  assert.throws(() => w.parties.invite(2, 'Whisper'), code('not_leader'));
  w.parties.invite(1, 'Whisper');
  w.parties.accept(3);
  w.parties.invite(1, 'Briar');
  w.parties.accept(4);
  assert.throws(() => w.parties.invite(1, 'Jack'), code('party_full'));
  assert.deepEqual(w.last(1, 'party').members.map((m: any) => m.colour), ['red', 'blue', 'yellow', 'green']);
  assert.throws(() => w.parties.kick(2, 'Briar'), code('not_leader'));
  w.parties.kick(1, 'Briar');
  assert.deepEqual(w.last(4, 'party'), { t: 'party', id: null });
  assert.ok(w.notes(4).includes('You were removed from the party.'));
  // The freed colour goes to the next one in.
  w.parties.invite(1, 'Jack');
  w.parties.accept(5);
  assert.equal(w.last(1, 'party').members.find((m: any) => m.name === 'Jack').colour, 'green');
  w.parties.lead(1, 'Whisper');
  assert.equal(w.last(2, 'party').leader, 3);
  assert.throws(() => w.parties.kick(1, 'Thunder'), code('not_leader'));
});

test('the lead passes on when the leader leaves, and a party of one breaks up', () => {
  const w = world();
  for (const [id, n] of [[1, 'Maze'], [2, 'Thunder'], [3, 'Whisper']] as const) w.join(id, n);
  w.parties.invite(1, 'Thunder');
  w.parties.accept(2);
  w.parties.invite(1, 'Whisper');
  w.parties.accept(3);
  w.parties.leave(1);
  assert.equal(w.last(2, 'party').leader, 2);
  assert.equal(w.last(2, 'party').members.length, 2);
  w.parties.leave(3);
  assert.deepEqual(w.last(2, 'party'), { t: 'party', id: null });
  assert.ok(w.notes(2).includes('Your party has broken up.'));
  assert.equal(w.parties.count, 0);
});

test('invites are refused for heroes in training, in a party, offline, or yourself; and run out', () => {
  const w = world({ training: [4] });
  for (const [id, n] of [[1, 'Maze'], [2, 'Thunder'], [3, 'Whisper'], [4, 'Cadet'], [5, 'Briar']] as const) w.join(id, n);
  assert.throws(() => w.parties.invite(1, 'Maze'), code('bad_target'));
  assert.throws(() => w.parties.invite(1, 'Nobody'), code('no_such_player'));
  assert.throws(() => w.parties.invite(1, 'Cadet'), code('in_training'));
  w.parties.invite(2, 'Whisper');
  w.parties.accept(3);
  assert.throws(() => w.parties.invite(1, 'Whisper'), code('in_a_party'));
  w.parties.invite(1, 'Briar');
  assert.throws(() => w.parties.accept(1), code('no_invite'));
  w.advance(61_000);
  w.parties.sweep();
  assert.ok(w.notes(1).some((n) => n.includes('ran out')));
  assert.throws(() => w.parties.accept(5), code('no_invite'));
});

test('accept picks the invite by name, and decline tells the inviter', () => {
  const w = world();
  for (const [id, n] of [[1, 'Maze'], [2, 'Thunder'], [3, 'Whisper']] as const) w.join(id, n);
  w.parties.invite(1, 'Whisper');
  w.parties.invite(2, 'Whisper');
  w.parties.decline(3, 'Thunder');
  assert.ok(w.notes(2).includes('Whisper declined your invite.'));
  w.parties.accept(3, 'Maze');
  assert.equal(w.parties.partyOf(3)!.leader, 1);
});

test('a dropped member keeps their place for the grace time, then leaves', () => {
  const w = world();
  for (const [id, n] of [[1, 'Maze'], [2, 'Thunder'], [3, 'Whisper']] as const) w.join(id, n);
  w.parties.invite(1, 'Thunder');
  w.parties.accept(2);
  w.parties.invite(1, 'Whisper');
  w.parties.accept(3);
  w.parties.disconnect(2);
  assert.equal(w.last(1, 'party').members.find((m: any) => m.id === 2).online, false);
  assert.deepEqual(w.parties.companions(1), [3]);
  w.advance(60_000);
  w.parties.sweep();
  w.join(2, 'Thunder'); // back, through a resume or a new login
  assert.equal(w.last(2, 'party').members.length, 3);
  assert.ok(w.notes(1).includes('Thunder is back.'));
  w.parties.disconnect(3);
  w.advance(5 * 60_000);
  w.parties.sweep();
  assert.equal(w.last(1, 'party').members.length, 2);
  assert.ok(w.notes(1).some((n) => n.includes('offline too long')));
});

test('a new member crosses to the channel their party is in', () => {
  const w = world();
  w.join(1, 'Maze', 'Bowerstone#1');
  w.join(2, 'Thunder', 'Bowerstone#2');
  w.parties.invite(1, 'Thunder');
  w.parties.accept(2);
  assert.deepEqual(w.moved, [[2, 'Bowerstone#1']]);
  // Already together: regroup says so and moves nobody.
  w.parties.regroup(2);
  assert.equal(w.moved.length, 1);
  assert.ok(w.notes(2).includes('You are already with your party.'));
});

// ------------------------------------------------------------ with the real Director

test('the Director places a hero with their party, past the soft cap', () => {
  const app = testApp({ channelSoftCap: 2, channelHardCap: 3, maxChannels: 4 });
  const hs = Array.from({ length: 4 }, () => makeHero(app).hero);
  const join = (h: (typeof hs)[number]) => {
    const ch = app.director.join(h, link(h.id, h.name, []));
    app.parties.connect(h.id, h.name, () => {});
    return ch;
  };
  const [a, b, c, d] = hs as [typeof hs[0], typeof hs[0], typeof hs[0], typeof hs[0]];
  const c1 = join(a);
  join(b);
  // The first channel is at the soft cap: a stranger goes to a new one.
  const c2 = join(c);
  assert.notEqual(c1, c2);
  // Invite the stranger: accepting moves them over, to the hard cap.
  app.parties.invite(a.id, c.name);
  app.parties.accept(c.id);
  assert.equal(app.director.channelOf(c.id), c1);
  assert.equal(c1.count, 3);
  // After a relog the party member lands in the party's channel again (it has room after c leaves).
  app.director.leave(c.id);
  app.parties.disconnect(c.id);
  join(d); // a stranger takes a channel: c1 has 2 = soft cap, so d goes elsewhere
  assert.notEqual(app.director.channelOf(d.id), c1);
  assert.equal(join(c), c1);
  app.close();
});

test('regroup sends a moved hero an enter_region with no load', () => {
  const app = testApp({ channelSoftCap: 1, channelHardCap: 4, maxChannels: 4 });
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  const inbox: string[] = [];
  const c1 = app.director.join(a, link(a.id, a.name, []));
  const c2 = app.director.join(b, link(b.id, b.name, inbox));
  assert.notEqual(c1, c2);
  app.parties.connect(a.id, a.name, () => {});
  app.parties.connect(b.id, b.name, () => {});
  app.parties.invite(a.id, b.name);
  app.parties.accept(b.id);
  const enter = inbox.map((f) => JSON.parse(f)).filter((m) => m.t === 'enter_region').at(-1);
  assert.equal(enter.channel, c1.id);
  assert.equal(enter.via, 'party');
  assert.equal(enter.loading, false);
  assert.ok(app.sink.events.some((e) => e.type === 'party_regroup'));
  app.close();
});

// ------------------------------------------------------------ over the game socket

test('parties over the game socket: invite, accept, party chat, and typed mistakes', async () => {
  const { listen } = await import('../src/net/server.js');
  const app = testApp({ tickRate: 30 });
  const server = await listen(app, 0);
  const port = (server.address() as import('node:net').AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Scythe');
    const a = await wsClient(`ws://127.0.0.1:${port}/play`);
    const b = await wsClient(`ws://127.0.0.1:${port}/play`);
    a.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket });
    b.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket });
    await a.next((m) => m.t === 'party' && m.id === null);
    await b.next((m) => m.t === 'party' && m.id === null);

    // A name with control characters or nobody behind it is a refusal, not a disconnect.
    a.send({ t: 'party', op: 'invite', name: 'Nob\u0007ody' });
    const err = await a.next((m) => m.t === 'party_error');
    assert.equal(err.error, 'no_such_player');

    a.send({ t: 'party', op: 'invite', name: 'scythe' });
    const inv = await b.next((m) => m.t === 'party_invite');
    assert.equal(inv.name, 'Mobdon');
    b.send({ t: 'party', op: 'accept' });
    const p = await a.next((m) => m.t === 'party' && m.id !== null);
    assert.deepEqual(p.members.map((m: any) => m.name), ['Mobdon', 'Scythe']);

    b.send({ t: 'party', op: 'say', text: 'ready' });
    const said = await a.next((m) => m.t === 'party_chat');
    assert.equal(said.name, 'Scythe');
    assert.equal(said.text, 'ready');

    // Scythe drops: Mobdon sees them offline, still in the party.
    b.ws.close();
    const off = await a.next((m) => m.t === 'party' && m.members?.some((x: any) => x.name === 'Scythe' && !x.online));
    assert.equal(off.members.length, 2);
    a.ws.close();
    await new Promise((r) => setTimeout(r, 100));
  } finally {
    server.close();
    app.close();
  }
});
