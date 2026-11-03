import { test } from "node:test";
import assert from "node:assert/strict";
import { ChatHub, type Conn } from "../src/hub.ts";
import { signToken } from "../src/auth.ts";
import { parseClientMsg, type ServerMsg, type ClientMsg } from "../src/protocol.ts";

class Fake implements Conn {
  got: ServerMsg[] = [];
  closedWith: string | null = null;
  send(m: ServerMsg) { this.got.push(m); }
  close(r: string) { this.closedWith = r; }
  of<T extends ServerMsg["t"]>(t: T) { return this.got.filter((m) => m.t === t) as Extract<ServerMsg, { t: T }>[]; }
  texts() { return this.of("msg").map((m) => `${m.ch}:${m.from}:${m.text}`); }
  errs() { return this.of("err").map((m) => m.code); }
}

function setup(cfg = {}) {
  let t = 1_000_000;
  const events: string[] = [];
  const hub = new ChatHub(cfg, (e) => events.push(e), () => t);
  const join = (name: string, pos?: [number, number, number, number]) => {
    const c = new Fake();
    hub.open(c);
    hub.handle(c, { t: "hello", v: 1, name });
    if (pos) hub.handle(c, { t: "pos", region: pos[0], x: pos[1], y: pos[2], z: pos[3] });
    return c;
  };
  const send = (c: Fake, m: ClientMsg) => hub.handle(c, m);
  return { hub, join, send, events, advance: (ms: number) => (t += ms) };
}

test("login gives welcome; bad and taken names are refused", () => {
  const { join } = setup();
  const a = join("Hal");
  assert.equal(a.of("welcome")[0].name, "Hal");
  const b = join("x");
  assert.deepEqual(b.errs(), ["bad_name"]);
  assert.equal(b.closedWith, "name");
});

test("token login in production mode", () => {
  const secret = "s3cret";
  const hub = new ChatHub({ tokenSecret: secret });
  const ok = new Fake(), bad = new Fake();
  const token = signToken({ sub: "acc1", name: "Maze", role: "player", renown: 1200, exp: Date.now() / 1000 + 60 }, secret);
  hub.handle(ok, { t: "hello", v: 1, token });
  assert.equal(ok.of("welcome")[0].renown, 1200);
  hub.handle(bad, { t: "hello", v: 1, token: token.slice(0, -2) + "xx" });
  assert.deepEqual(bad.errs(), ["auth_failed"]);
  const noName = new Fake();
  hub.handle(noName, { t: "hello", v: 1, name: "Maze2" });  // names alone don't work with a secret
  assert.deepEqual(noName.errs(), ["auth_failed"]);
});

test("say reaches only nearby players in the same region", () => {
  const { join, send } = setup();
  const a = join("Alpha", [5, 0, 0, 0]);
  const near = join("Bravo", [5, 10, 0, 0]);
  const far = join("Charlie", [5, 100, 0, 0]);
  const other = join("Delta", [6, 0, 0, 0]);
  send(a, { t: "say", ch: "say", text: "hello" });
  assert.deepEqual(a.texts(), ["say:Alpha:hello"]);
  assert.deepEqual(near.texts(), ["say:Alpha:hello"]);
  assert.deepEqual(far.texts(), []);
  assert.deepEqual(other.texts(), []);
  send(a, { t: "say", ch: "shout", text: "HEY" });
  assert.deepEqual(far.texts(), ["shout:Alpha:HEY"]);
  assert.deepEqual(other.texts(), []);
  send(a, { t: "say", ch: "world", text: "anyone?" });
  assert.deepEqual(other.texts(), ["world:Alpha:anyone?"]);
});

test("whisper, reply target and ignore", () => {
  const { join, send } = setup();
  const a = join("Alpha"), b = join("Bravo");
  send(a, { t: "whisper", to: "bravo", text: "psst" });
  assert.deepEqual(b.texts(), ["whisper:Alpha:psst"]);
  assert.equal(b.of("msg")[0].to, "Bravo");
  send(b, { t: "ignore", name: "Alpha", on: true });
  send(a, { t: "whisper", to: "Bravo", text: "again" });
  assert.equal(b.of("msg").length, 1);       // ignored: not delivered
  assert.equal(a.of("msg").length, 2);       // sender sees it as sent either way
  send(a, { t: "whisper", to: "Nobody", text: "hi" });
  assert.deepEqual(a.errs(), ["no_such_player"]);
});

test("party and guild need membership", () => {
  const { hub, join, send } = setup();
  const a = join("Alpha"), b = join("Bravo"), c = join("Charlie");
  send(a, { t: "say", ch: "party", text: "x" });
  assert.deepEqual(a.errs(), ["not_in_party"]);
  hub.setParty("Alpha", "p1"); hub.setParty("Bravo", "p1");
  send(a, { t: "say", ch: "party", text: "go" });
  assert.deepEqual(b.texts(), ["party:Alpha:go"]);
  assert.deepEqual(c.texts(), []);
});

test("flood control, duplicates and automatic mute", () => {
  const { join, send, advance, events } = setup();
  const a = join("Alpha");
  send(a, { t: "say", ch: "world", text: "one" });
  send(a, { t: "say", ch: "world", text: "two" });
  assert.deepEqual(a.errs(), ["rate_limited"]);   // world cooldown 10 s
  advance(11_000);
  send(a, { t: "say", ch: "world", text: "one" });
  assert.deepEqual(a.errs(), ["rate_limited", "duplicate"]);
  for (let i = 0; i < 10; i++) send(a, { t: "say", ch: "trade", text: "WTS " + i });
  assert.ok(a.errs().includes("muted"));
  assert.ok(events.includes("chat.automute"));
  advance(60_000);
  send(a, { t: "say", ch: "trade", text: "still muted?" });
  assert.equal(a.errs().at(-1), "muted");
  advance(5 * 60_000);
  send(a, { t: "say", ch: "trade", text: "back" });
  assert.equal(a.texts().at(-1), "trade:Alpha:back");
});

test("text is cleaned and filtered", () => {
  const { join, send } = setup({ bannedWords: ["darn"] });
  const a = join("Alpha");
  send(a, { t: "say", ch: "world", text: "  oh‮  d4rn\u0007 it  " });
  assert.deepEqual(a.texts(), ["world:Alpha:oh **** it"]);
  send(a, { t: "say", ch: "shout", text: "x".repeat(201) });
  assert.deepEqual(a.errs(), ["too_long"]);
});

test("moderators can mute and kick; players can't", () => {
  const { join, send } = setup();
  const mod = join("Mod Kay"), p = join("Pest"), q = join("Quiet");
  send(q, { t: "mod", action: "mute", name: "Pest" });
  assert.deepEqual(q.errs(), ["forbidden"]);
  send(mod, { t: "mod", action: "mute", name: "Pest", minutes: 10, reason: "spam" });
  send(p, { t: "say", ch: "world", text: "hi" });
  assert.deepEqual(p.errs(), ["muted"]);
  send(mod, { t: "mod", action: "kick", name: "Pest" });
  assert.equal(p.closedWith, "kicked");
});

test("name tags: add, move, update, delete as players come and go", () => {
  const { hub, join, send } = setup();
  const a = join("Alpha", [5, 0, 0, 0]);
  const b = join("Bravo", [5, 20, 0, 0]);
  hub.tick();
  const t1 = a.of("tags")[0];
  assert.equal(t1.add.length, 1);
  assert.equal(t1.add[0].name, "Bravo");
  assert.equal(t1.add[0].renown, 0);
  hub.tick();
  assert.equal(a.of("tags").length, 1);   // nothing changed: nothing sent
  send(b, { t: "pos", region: 5, x: 25, y: 0, z: 1 });
  hub.tick();
  assert.deepEqual(a.of("tags")[1].move, [{ id: t1.add[0].id, x: 25, y: 0, z: 1 }]);
  hub.updateHero("Bravo", { renown: 5000, title: "Arrow Master", pvp: true });
  hub.tick();
  const u = a.of("tags")[2].upd[0];
  assert.equal(u.renown, 5000); assert.equal(u.title, "Arrow Master"); assert.equal(u.pvp, true);
  send(b, { t: "pos", region: 5, x: 500, y: 0, z: 0 });   // walks out of range
  hub.tick();
  assert.deepEqual(a.of("tags")[3].del, [t1.add[0].id]);
  send(b, { t: "pos", region: 5, x: 10, y: 0, z: 0 });
  hub.tick();
  hub.closed(b);
  assert.deepEqual(a.of("tags").at(-1)!.del, [t1.add[0].id]);
});

test("client positions are ignored when the region server owns them", () => {
  const { hub, join } = setup({ trustClientPositions: false });
  const a = join("Alpha", [5, 0, 0, 0]);
  join("Bravo", [5, 1, 0, 0]);
  hub.tick();
  assert.equal(a.of("tags").length, 0);
  hub.updateHero("Alpha", { region: 5, x: 0, y: 0, z: 0 });
  hub.updateHero("Bravo", { region: 5, x: 1, y: 0, z: 0 });
  hub.tick();
  assert.equal(a.of("tags")[0].add[0].name, "Bravo");
});

test("parser rejects junk", () => {
  assert.equal(parseClientMsg("not json"), null);
  assert.equal(parseClientMsg('{"t":"say","ch":"admin","text":"x"}'), null);
  assert.equal(parseClientMsg('{"t":"pos","region":1,"x":"1","y":0,"z":0}'), null);
  assert.equal(parseClientMsg("[1,2]"), null);
  assert.deepEqual(parseClientMsg('{"t":"ping","n":3}'), { t: "ping", n: 3 });
});

test("dev hello can set tag fields; token login ignores them", () => {
  const { hub, join } = setup();
  const c = new Fake();
  hub.handle(c, { t: "hello", v: 1, name: "Bot One", dev: { renown: 9000, title: "Hero", guild: "OAK", pvp: true } });
  hub.updateHero("Bot One", { region: 1, x: 0, y: 0, z: 0 });
  const a = join("Alpha", [1, 1, 0, 0]);
  hub.tick();
  const t = a.of("tags")[0].add[0];
  assert.deepEqual([t.renown, t.title, t.guild, t.pvp], [9000, "Hero", "OAK", true]);
  const prod = new ChatHub({ tokenSecret: "k" });
  const p = new Fake();
  const token = signToken({ sub: "a", name: "Real", role: "player", exp: Date.now() / 1000 + 60 }, "k");
  prod.handle(p, { t: "hello", v: 1, token, dev: { renown: 99999 } });
  assert.equal(p.of("welcome")[0].renown, 0);
});
