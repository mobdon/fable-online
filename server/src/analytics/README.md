# Server analytics

Metrics, event log and economy watchdog, inside the game server. Prometheus scrapes it;
Grafana shows it (dashboards and alert rules live in [`ops/monitoring`](../../../ops/monitoring)).

The server's own systems don't call the metrics directly. They emit `GameEvent`s
(`events.ts`) as they always have, and `createApp` sends every event both to the raw game event
log (`FO_EVENTS`) and to `AnalyticsBridge` (`bridge.ts`), which turns them into `Analytics` calls:

| Game event | Analytics |
| --- | --- |
| `login` | `fable_logins_total`, unique accounts today |
| `player_connected`, `region_enter`, `player_disconnected` | heroes online per region, region transfers, session length |
| `gold` (ledger) | gold minted per source, burned per sink, transferred; every one checked by the watchdog |
| `item` | items created and destroyed; Resurrection Phials created (`phial_rate`) |
| `hero_death` with a phial, `pvp_kill` | phials used, PvP kills |
| `chat` (Realm chat) | chat messages per channel, player reports |

The ledger's reasons are renamed to the allow-list's (`loot` is `monster_drop`, `vendor_sell` is
`shop_sell`, `vendor_buy` is `shop_buy`...). The app also reports tick time every frame, open
world and chat connections, and every minute compares the ledger's gold supply with the wallets.

Settings (`server/src/config.ts`): `FO_METRICS_PORT` (9464, 0 = off) and `FO_METRICS_HOST`
(127.0.0.1: `/metrics` must never be reachable from the internet), `FO_ANALYTICS_DIR`
(`data/analytics`, one `events-YYYY-MM-DD.jsonl` per UTC day, '' = off). Watchdog anomalies
are printed to the server console too.

## Two outputs, on purpose

| | Prometheus metrics | Event log (`events-YYYY-MM-DD.jsonl`) |
| --- | --- | --- |
| Holds | Totals and rates: heroes online per region, gold minted per source, win rate per school | Every single event with the hero ids |
| Used for | Dashboards and alerts | Finding *which* hero tripped an alert, history, later loading into ClickHouse or Loki |

Prometheus labels are kept to small fixed sets (`bounded()` in `metrics.ts` turns
anything unknown into `other`), so a bug or a hostile client can't blow up the metric count.

## Economy watchdog

Every gold change goes through `goldMinted`, `goldBurned` or `goldTransferred`. Each is
checked against these rules (`watchdog.ts`). The watchdog only reports; the server decides what to do.

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
cd server
npm test                  # includes test/analytics.test.ts and test/realm-chat.test.ts
npm run analytics-demo    # fake traffic on :9464/metrics so the dashboards have data
```
