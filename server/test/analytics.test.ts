import { test } from "node:test";
import assert from "node:assert/strict";
import { Analytics } from "../src/analytics/analytics.js";
import { EconomyWatchdog } from "../src/analytics/watchdog.js";
import { MemorySink } from "../src/analytics/event-log.js";

const rules = (xs: { rule: string }[]) => xs.map((x) => x.rule).sort();

test("normal gold flow raises nothing", () => {
  const w = new EconomyWatchdog();
  assert.deepEqual(w.checkGold({ playerId: "a", delta: 500, kind: "mint", reason: "quest_reward", txnId: "1", balanceBefore: 100, balanceAfter: 600 }), []);
  assert.deepEqual(w.checkGold({ playerId: "a", delta: 200, kind: "burn", reason: "shop_buy", txnId: "2", balanceBefore: 600, balanceAfter: 400 }), []);
});

test("replayed transaction id is a dupe", () => {
  const w = new EconomyWatchdog();
  w.checkGold({ playerId: "a", delta: 100, kind: "mint", reason: "shop_sell", txnId: "x" });
  assert.deepEqual(rules(w.checkGold({ playerId: "a", delta: 100, kind: "mint", reason: "shop_sell", txnId: "x" })), ["duplicate_txn"]);
});

test("unknown source, unknown sink, oversized mint", () => {
  const w = new EconomyWatchdog();
  assert.deepEqual(rules(w.checkGold({ playerId: "a", delta: 10, kind: "mint", reason: "npc_gift" })), ["unknown_source"]);
  assert.deepEqual(rules(w.checkGold({ playerId: "a", delta: 10, kind: "burn", reason: "void" })), ["unknown_sink"]);
  assert.deepEqual(rules(w.checkGold({ playerId: "b", delta: 25_000, kind: "mint", reason: "chest" })), ["large_delta"]);
  assert.deepEqual(w.checkGold({ playerId: "c", delta: 25_000, kind: "mint", reason: "quest_reward" }), []);
  assert.deepEqual(w.checkGold({ playerId: "d", delta: 1e9, kind: "mint", reason: "gm_grant" }), []);
});

test("balance that doesn't add up, or goes negative", () => {
  const w = new EconomyWatchdog();
  assert.deepEqual(rules(w.checkGold({ playerId: "a", delta: 100, kind: "mint", reason: "chest", balanceBefore: 0, balanceAfter: 1100 })), ["balance_mismatch"]);
  assert.deepEqual(rules(w.checkGold({ playerId: "a", delta: 100, kind: "burn", reason: "tavern", balanceBefore: 50, balanceAfter: -50 })), ["negative_balance"]);
});

test("earn rate fires once when crossing the limit, and resets after the window", () => {
  let now = 0;
  const w = new EconomyWatchdog({ maxEarnPerWindow: 1000, earnWindowMs: 60_000 }, () => now);
  const mint = () => rules(w.checkGold({ playerId: "farmer", delta: 400, kind: "mint", reason: "monster_drop" }));
  assert.deepEqual(mint(), []);
  assert.deepEqual(mint(), []);
  assert.deepEqual(mint(), ["earn_rate"]); // 1200 > 1000
  assert.deepEqual(mint(), []);            // already flagged this window
  now = 120_000;
  assert.deepEqual(mint(), []);
  assert.equal(w.flaggedCount(), 1);
});

test("phial rate, self trade, supply drift", () => {
  const w = new EconomyWatchdog({ maxPhialsPerWindow: 2 });
  w.checkPhial("a"); w.checkPhial("a");
  assert.deepEqual(rules(w.checkPhial("a")), ["phial_rate"]);
  assert.deepEqual(rules(w.checkTransfer({ fromPlayerId: "a", toPlayerId: "a", amount: 5 })), ["self_trade"]);
  assert.deepEqual(w.checkSupply(1000, 1000), []);
  assert.deepEqual(rules(w.checkSupply(1000, 1500)), ["supply_drift"]);
});

test("Analytics keeps ledger supply, metrics and event log in step", async () => {
  const sink = new MemorySink();
  const seen: string[] = [];
  const a = new Analytics({ sink, defaultMetrics: false, onAnomaly: (x) => seen.push(x.rule) });
  a.setLedgerSupply(1000);
  a.goldMinted({ playerId: "a", amount: 300, source: "quest_reward", region: "oakvale", txnId: "1" });
  a.goldBurned({ playerId: "a", amount: 100, sink: "shop_buy", region: "oakvale", txnId: "2" });
  a.goldMinted({ playerId: "a", amount: 300, source: "quest_reward", region: "oakvale", txnId: "1" });
  assert.equal(a.goldSupply, 1500);
  a.reportDbSupply(1500);
  assert.deepEqual(seen, ["duplicate_txn"]);
  assert.equal(sink.ofType("economy.gold").length, 3);
  assert.equal(sink.ofType("economy.anomaly").length, 1);

  const text = await a.metrics.registry.metrics();
  assert.match(text, /fable_gold_minted_total\{source="quest_reward",region="oakvale"\} 600/);
  assert.match(text, /fable_gold_supply_ledger 1500/);
  assert.match(text, /fable_economy_anomalies_total\{rule="duplicate_txn"\} 1/);
});

test("players online follows region moves and logouts", async () => {
  let now = 0;
  const a = new Analytics({ defaultMetrics: false, now: () => now });
  a.login("acc1", "ok");
  a.enterRegion("h1", "oakvale");
  a.enterRegion("h2", "oakvale");
  a.enterRegion("h1", "bowerstone_market", "cullis_gate");
  now = 90_000;
  a.logout("h2");
  const text = await a.metrics.registry.metrics();
  assert.match(text, /fable_players_online\{region="oakvale"\} 0/);
  assert.match(text, /fable_players_online\{region="bowerstone_market"\} 1/);
  assert.match(text, /fable_cullis_teleports_total\{from="oakvale",to="bowerstone_market"\} 1/);
  assert.match(text, /fable_session_duration_seconds_count 1/);
  assert.match(text, /fable_players_unique_today 1/);
});

test("unknown labels are clamped to 'other'", async () => {
  const a = new Analytics({ defaultMetrics: false });
  a.pvpHit({ venue: "arena", attackerId: "a", victimId: "b", ability: "<script>", damage: 10 });
  const text = await a.metrics.registry.metrics();
  assert.match(text, /fable_pvp_ability_uses_total\{venue="arena",ability="other"\} 1/);
});

test("earn_outlier learns normal play and flags a hero far above it", () => {
  let now = 0;
  const w = new EconomyWatchdog({ outlierMinSamples: 50, outlierFloor: 1000, outlierMultiplier: 5 }, () => now);
  // Hour 0: 100 ordinary heroes earn 100..1090 gold. Still learning, nothing flagged.
  for (let i = 0; i < 100; i++) w.checkGold({ playerId: `h${i}`, delta: 100 + i * 10, kind: "mint", reason: "quest_reward" });
  assert.equal(w.outlierLimit(), undefined);
  // Hour 1: baseline p99 is 1090, so the limit is 5450.
  now = 3_600_000;
  assert.equal(w.baselineP99, 1090);
  assert.equal(w.outlierLimit(), 5450);
  assert.deepEqual(w.drainClosedHours().length, 100);
  assert.deepEqual(w.checkGold({ playerId: "normal", delta: 900, kind: "mint", reason: "quest_reward" }), []);
  assert.deepEqual(w.checkGold({ playerId: "farmer", delta: 5000, kind: "mint", reason: "monster_drop" }), []);
  assert.deepEqual(rules(w.checkGold({ playerId: "farmer", delta: 1000, kind: "mint", reason: "monster_drop" })), ["earn_outlier"]);
  assert.deepEqual(w.checkGold({ playerId: "farmer", delta: 1000, kind: "mint", reason: "monster_drop" }), []); // once per hour
  // GM grants don't count.
  assert.deepEqual(w.checkGold({ playerId: "gm", delta: 1e6, kind: "mint", reason: "gm_grant" }), []);
});
