// Fake traffic so the Grafana dashboards have something to show before the real
// server exists. Not used in production.
//
//   npm run demo                      -> metrics on http://0.0.0.0:9464/metrics
//   DEMO_PLAYERS=800 npm run demo     -> more heroes
//
// One hero ("exploiter") farms gold far too fast so the economy alerts fire.

import { Analytics } from "../src/analytics/analytics.js";
import { JsonlFileSink } from "../src/analytics/event-log.js";
import { startMetricsServer } from "../src/analytics/http.js";
import { ABILITIES, GOLD_SINKS, GOLD_SOURCES, SCHOOLS, type School } from "../src/analytics/metrics.js";

const REGIONS = [
  "bowerstone_market", "bowerstone_south", "oakvale", "knothole_glade", "greatwood",
  "barrow_fields", "witchwood", "darkwood", "twinblade_camp", "hook_coast", "arena", "guild",
];
const PLAYERS = Number(process.env.DEMO_PLAYERS ?? 300);
const pick = <T>(xs: readonly T[]): T => xs[Math.floor(Math.random() * xs.length)]!;
const rand = (a: number, b: number) => a + Math.random() * (b - a);

const a = new Analytics({
  sink: new JsonlFileSink(process.env.DEMO_EVENTS_DIR ?? "./events"),
  onAnomaly: (x) => console.log(`[watchdog] ${x.rule} ${x.playerId ?? ""} ${JSON.stringify(x.detail)}`),
});
a.setLedgerSupply(5_000_000);
for (const r of REGIONS) a.regionProcess(r, true);

const heroes = Array.from({ length: PLAYERS }, (_, i) => ({ id: `hero${i}`, school: pick(SCHOOLS) as School, online: false }));
let txn = 0;
let dbDrift = 0;
let dupe = 0;

function step() {
  // logins / logouts / region changes
  for (const h of heroes) {
    if (!h.online && Math.random() < 0.02) { a.login(h.id, Math.random() < 0.03 ? "bad_password" : "ok"); a.enterRegion(h.id, pick(REGIONS)); h.online = true; }
    else if (h.online && Math.random() < 0.005) { a.logout(h.id); h.online = false; }
    else if (h.online && Math.random() < 0.01) a.enterRegion(h.id, pick(REGIONS), Math.random() < 0.3 ? "cullis_gate" : "walk");
  }
  const online = heroes.filter((h) => h.online);
  a.setTutorialCount(Math.floor(rand(5, 25)));
  a.setPvpFlaggedCount(Math.floor(online.length * 0.4));

  // economy
  for (let i = 0; i < online.length / 3; i++) {
    const h = pick(online);
    a.goldMinted({ playerId: h.id, amount: Math.round(rand(5, 800)), source: pick(GOLD_SOURCES.filter((s) => s !== "gm_grant")), region: pick(REGIONS), txnId: `t${txn++}` });
    a.goldBurned({ playerId: h.id, amount: Math.round(rand(5, 900)), sink: pick(GOLD_SINKS.filter((s) => s !== "gm_remove")), region: pick(REGIONS), txnId: `t${txn++}` });
    const partner = pick(online);
    if (Math.random() < 0.2 && partner !== h) a.goldTransferred({ fromPlayerId: h.id, toPlayerId: partner.id, amount: Math.round(rand(10, 2000)), kind: "trade", txnId: `t${txn++}` });
    if (Math.random() < 0.1) a.itemCreated({ playerId: h.id, itemDef: "OBJECT_HEALTH_POTION", category: "potion", source: "shop" });
  }
  if (Math.random() < 0.02 && online.length) a.phialCreated(pick(online).id, "quest_reward");
  if (Math.random() < 0.015 && online.length) a.phialUsed(pick(online).id, pick(["arena", "open_world", "pve"] as const));
  if (Math.random() < 0.05 && online.length) a.itemDestroyed({ playerId: pick(online).id, itemDef: "OBJECT_HEALTH_POTION", category: "potion", sink: "consumed" });
  if (Math.random() < 0.02 && online.length > 1) a.playerReport(pick(online).id, pick(online).id, pick(["spam", "abuse", "cheating"] as const));
  // the exploiter: replays a shop sale packet
  if (Math.random() < 0.3) {
    const id = `dupe${Math.floor(dupe++ / 3)}`; // every sale is replayed twice
    a.goldMinted({ playerId: "exploiter", amount: 4_000, source: "shop_sell", region: "bowerstone_market", txnId: id });
  }
  if (Math.random() < 0.01) a.goldMinted({ playerId: "hero_weird", amount: 500, source: "unknown_npc_gift", region: "oakvale" });
  if (Math.random() < 0.01) dbDrift += 1_000; // gold appearing outside the ledger

  // PvP
  for (let i = 0; i < online.length / 10; i++) {
    const [x, y] = [pick(online), pick(online)];
    if (x === y) continue;
    const venue = Math.random() < 0.6 ? "arena" : "open_world";
    const ability = x.school === "will" ? pick(ABILITIES.slice(3)) : x.school === "melee" ? "melee_attack" : "ranged_attack";
    a.pvpHit({ venue, attackerId: x.id, victimId: y.id, ability, damage: Math.round(rand(5, ability === "fireball" ? 90 : 50)), countered: Math.random() < 0.25 ? pick(["block", "dodge", "roll", "physical_shield"]) : undefined });
    if (Math.random() < 0.1) a.pvpKill({ venue, killerId: x.id, victimId: y.id, killerSchool: x.school, victimSchool: y.school, secondsToKill: rand(2, x.school === "will" ? 12 : 25), region: venue === "arena" ? "arena" : pick(REGIONS) });
  }
  if (Math.random() < 0.3 && online.length > 2) {
    const [x, y] = [pick(online), pick(online)];
    const willEdge = (s: School) => (s === "will" ? 0.15 : 0); // a deliberately unbalanced school, so the dashboard shows it
    const xWins = Math.random() + willEdge(x.school) - willEdge(y.school) > 0.5;
    a.arenaMatch({ matchId: `m${txn}`, mode: "1v1", seconds: rand(40, 400), players: [
      { playerId: x.id, school: x.school, result: xWins ? "win" : "loss", queueSeconds: rand(5, 120) },
      { playerId: y.id, school: y.school, result: xWins ? "loss" : "win", queueSeconds: rand(5, 120) },
    ] });
  }

  // health
  for (const r of REGIONS) a.tick(r, Math.abs(rand(0.004, r === "bowerstone_market" ? 0.04 : 0.02)));
  if (Math.random() < 0.003) { const r = pick(REGIONS); a.regionProcess(r, true, "crash"); }
  a.metrics.connections.set({ service: "gateway" }, online.length);
  for (const r of REGIONS) a.metrics.rttSeconds.observe({ region: r }, rand(0.03, 0.2));
  for (let i = 0; i < online.length; i++) a.net("in", "InputFrame", 24);
  a.net("out", "WorldTick", online.length * 400);
  for (let i = 0; i < online.length / 20; i++) a.chat(pick(["say", "region", "whisper", "trade"] as const));
  void a.timeDb("save_hero", () => new Promise((res) => setTimeout(res, rand(1, 30))));
}

// Reported DB supply = ledger + any off-ledger drift.
const reportSupply = () => a.reportDbSupply(a.goldSupply + dbDrift);

const port = Number(process.env.METRICS_PORT ?? 9464);
await startMetricsServer(a.metrics.registry, { port, host: process.env.METRICS_HOST ?? "0.0.0.0" });
console.log(`demo: ${PLAYERS} heroes, metrics on :${port}/metrics`);
setInterval(step, 1000);
setInterval(reportSupply, 15_000);
