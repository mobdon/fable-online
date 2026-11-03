# @fable-online/analytics

Metrics, event log and economy watchdog for the Fable Online server. The gateway and
the region orchestrators import it; Prometheus scrapes it; Grafana shows it
(dashboards and alert rules live in [`ops/monitoring`](../../ops/monitoring)).

```ts
import { Analytics, JsonlFileSink, startMetricsServer } from "@fable-online/analytics";

const analytics = new Analytics({
  sink: new JsonlFileSink("/var/log/fable/events"),   // one JSON line per event, per UTC day
  onAnomaly: (a) => gm.alert(a),                        // your call: log, freeze the hero, page a GM
});
await startMetricsServer(analytics.metrics.registry, { port: 9464, host: "10.0.0.5" }); // private address only

analytics.setLedgerSupply(await db.sumAllGold());      // on startup
analytics.enterRegion(heroId, "oakvale");
analytics.goldMinted({ playerId: heroId, amount: 250, source: "quest_reward", region: "oakvale", txnId, balanceBefore, balanceAfter });
analytics.pvpKill({ venue: "arena", killerId, victimId, killerSchool: "will", victimSchool: "melee", secondsToKill: 8, region: "arena" });
setInterval(async () => analytics.reportDbSupply(await db.sumAllGold()), 60_000);
```

## Two outputs, on purpose

| | Prometheus metrics | Event log (`events-YYYY-MM-DD.jsonl`) |
| --- | --- | --- |
| Holds | Totals and rates: heroes online per region, gold minted per source, win rate per school | Every single event with the hero ids |
| Used for | Dashboards and alerts | Finding *which* hero tripped an alert, history, later loading into ClickHouse or Loki |

Prometheus labels are kept to small fixed sets (`bounded()` in `metrics.ts` turns
anything unknown into `other`), so a bug or a hostile client can't blow up the metric count.

## Economy watchdog

Every gold change goes through `goldMinted`, `goldBurned` or `goldTransferred`. Each is
checked against these rules (`src/economy.ts`). The watchdog only reports; the server decides what to do.

| Rule | Catches |
| --- | --- |
| `duplicate_txn` | The same transaction applied twice: replayed packets, dupe bugs |
| `balance_mismatch` | `before + delta != after`: the balance changed off-ledger |
| `supply_drift` | Sum of all DB balances differs from minted minus burned |
| `unknown_source` / `unknown_sink` | Gold created or destroyed by code that isn't on the allowed list |
| `large_delta` | One payout bigger than that source should ever give |
| `earn_outlier` | One hero earning 5x what the top 1% of heroes normally earn in an hour. Learns "normal" from real play, so it needs no hand-set number (switches on after 200 hero-hours) |
| `earn_rate` | One hero earning more than 100,000 gold an hour (fixed backstop, draft limit) |
| `negative_balance` | A balance below zero |
| `phial_rate` | More than 2 Resurrection Phials per hero per day (draft limit) |
| `self_trade` | A hero trading with themselves |

Limits are in `DEFAULT_WATCHDOG_CONFIG` and can be overridden per server. Gold income
per hero has no reference data before launch, so `earn_outlier` derives it: every finished hour, each
hero's earnings go into `fable_gold_earned_per_hero_hour`, and the Economy dashboard's
"What normal play looks like" row shows the typical, top 10% and top 1% hero. After the
first week, use those numbers to set the fixed limits.

## Commands

```
npm install
npm test          # unit tests (node:test)
npm run demo      # fake traffic on :9464/metrics so the dashboards have data
npm run build     # compile to dist/
```
