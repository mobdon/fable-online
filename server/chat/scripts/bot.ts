// A fake player for testing chat and name tags without a second copy of Fable.
//   npm run bot -- --name "Test Bot" --region 12 --at 100,20,5 --renown 2500 --title "Hero" --guild OAK [--pvp]
// (renown, title, guild and pvp only work on a dev server: production takes them from the account)
// Walks in a small circle at --at and answers whispers. Type lines to chat as the bot
// ("/w Hal hi", "/world hello", or plain text for say).
import { connect } from "node:net";
import { createInterface } from "node:readline";

const arg = (k: string, d: string) => { const i = process.argv.indexOf("--" + k); return i > 0 ? process.argv[i + 1] : d; };
const host = arg("host", "127.0.0.1"), port = Number(arg("port", "7781"));
const name = arg("name", "Test Bot"), region = Number(arg("region", "0"));
const [cx, cy, cz] = arg("at", "0,0,0").split(",").map(Number);
const radius = Number(arg("circle", "3"));

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
send({ t: "hello", v: 1, name, dev: { renown: Number(arg("renown", "0")), title: arg("title", ""), guild: arg("guild", ""), pvp: process.argv.includes("--pvp") } });

let a = 0;
setInterval(() => {
  a += 0.15;
  send({ t: "pos", region, x: cx + Math.cos(a) * radius, y: cy + Math.sin(a) * radius, z: cz });
}, 200);
setInterval(() => send({ t: "ping", n: Date.now() }), 30_000);

createInterface({ input: process.stdin }).on("line", (l) => {
  const w = /^\/w\s+(\S+)\s+(.+)/.exec(l);
  const c = /^\/(say|region|world|trade|party|guild|1|2)\s+(.+)/.exec(l);
  const alias: Record<string, string> = { "1": "world", "2": "trade" };
  if (w) send({ t: "whisper", to: w[1], text: w[2] });
  else if (c) send({ t: "say", ch: alias[c[1]] ?? c[1], text: c[2] });
  else if (l.trim()) send({ t: "say", ch: "say", text: l });
});
console.log(`bot ${name} at region ${region} (${cx}, ${cy}, ${cz})`);
