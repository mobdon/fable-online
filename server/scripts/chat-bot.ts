// A fake player for testing Realm chat without a second copy of Fable.
//   npm run chat-bot -- --user Maze --password password123 [--hero Maze] [--server http://127.0.0.1:7777]
// Logs in through the launcher API like a real player (registering the account and making the
// hero if they don't exist yet), takes the chat token from /join and connects to Realm chat with
// it. It never enters the world, so it has no name tag; use `npm run walkers` for that.
// Answers whispers. Type lines to chat as the bot ("/w Hal hi", "/realm hello", or plain text for say).
import { connect } from "node:net";
import { createInterface } from "node:readline";

const arg = (k: string, d: string) => { const i = process.argv.indexOf("--" + k); return i > 0 ? process.argv[i + 1] ?? d : d; };
const base = arg("server", "http://127.0.0.1:7777").replace(/\/+$/, "");
const user = arg("user", "chat_bot"), password = arg("password", "password123");
const heroName = arg("hero", "Chatbot");

async function api(method: string, path: string, body?: object, token?: string): Promise<any> {
  const res = await fetch(base + path, {
    method,
    headers: { "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: res.status, ...((await res.json().catch(() => ({}))) as object) };
}

let login = await api("POST", "/launcher/v1/login", { username: user, password });
if (login.status === 401) login = await api("POST", "/launcher/v1/register", { username: user, password });
if (!login.access_token) { console.error(`login failed: ${login.message ?? login.error ?? login.status}`); process.exit(1); }
const heroes = await api("GET", "/launcher/v1/heroes", undefined, login.access_token);
if (!heroes.heroes?.length) {
  const made = await api("POST", "/launcher/v1/heroes", { name: heroName }, login.access_token);
  if (!made.hero) { console.error(`could not make hero ${heroName}: ${made.message ?? made.error}`); process.exit(1); }
}
const join = await api("POST", "/launcher/v1/join", { client_version: "9.9.9" }, login.access_token);
if (!join.chat?.token) { console.error(`no chat login from /join: ${join.message ?? join.error ?? "server runs without chat"}`); process.exit(1); }
const { host, port, token } = join.chat as { host: string; port: number; token: string };
const name = join.hero?.name ?? heroName;

const sock = connect(port, host);
const send = (o: unknown) => sock.write(JSON.stringify(o) + "\n");
sock.setEncoding("utf8");
let buf = "";
sock.on("data", (d: string) => {
  buf += d;
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const m = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
    if (m.t === "msg") console.log(`[${m.ch}] ${m.from}${m.to ? " -> " + m.to : ""}: ${m.text}`);
    else if (m.t === "tags") { for (const a of m.add) console.log(`tag in range: ${a.name} (${a.renown} renown)`); }
    else console.log(JSON.stringify(m));
    if (m.t === "msg" && m.ch === "whisper" && m.from !== name) send({ t: "whisper", to: m.from, text: `I hear you, ${m.from}.` });
  }
});
sock.on("close", () => process.exit(0));
sock.on("error", (e) => { console.error(`can't reach the chat server at ${host}:${port}: ${e.message}`); process.exit(1); });
send({ t: "hello", v: 1, token });
setInterval(() => send({ t: "ping", n: Date.now() }), 30_000);

createInterface({ input: process.stdin }).on("line", (l) => {
  const w = /^\/w\s+(\S+)\s+(.+)/.exec(l);
  const c = /^\/(say|region|realm|world|trade|party|guild|1|2)\s+(.+)/.exec(l);
  const alias: Record<string, string> = { "1": "realm", world: "realm", "2": "trade" };
  if (w) send({ t: "whisper", to: w[1], text: w[2] });
  else if (c) send({ t: "say", ch: alias[c[1]] ?? c[1], text: c[2] });
  else if (l.trim()) send({ t: "say", ch: "say", text: l });
});
console.log(`chat bot ${name} on ${host}:${port}`);
