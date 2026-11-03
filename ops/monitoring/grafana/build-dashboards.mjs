// Generates the Grafana dashboards in ./dashboards from code, so they stay
// reviewable and consistent. Run after editing:  node grafana/build-dashboards.mjs
import { writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const OUT = join(dirname(fileURLToPath(import.meta.url)), "dashboards");
const DS = { type: "prometheus", uid: "prometheus" };

let id = 0;
const target = (expr, legendFormat = "", extra = {}) => ({ datasource: DS, expr, legendFormat, refId: String.fromCharCode(65 + (id++ % 26)), ...extra });

function panel(type, title, gridPos, targets, { unit, description, stack, min, max, decimals, thresholds, options = {}, overrides = [], fieldConfig = {}, transformations } = {}) {
  const { custom = {}, ...restField } = fieldConfig;
  return {
    id: ++id, type, title, description, datasource: DS, gridPos, transformations,
    targets: targets.map((t) => (typeof t === "string" ? target(t) : t)),
    fieldConfig: {
      defaults: {
        unit, min, max, decimals,
        thresholds: { mode: "absolute", steps: thresholds ?? [{ color: "text", value: null }] },
        ...(thresholds && type !== "timeseries" ? { color: { mode: "thresholds" } } : {}),
        custom: { ...(type === "timeseries" ? { drawStyle: "line", lineWidth: 2, fillOpacity: stack ? 30 : 10, stacking: { mode: stack ? "normal" : "none" }, showPoints: "never" } : {}), ...custom },
        ...restField,
      },
      overrides,
    },
    options: type === "timeseries"
      ? { legend: { displayMode: "list", placement: "bottom" }, tooltip: { mode: "multi", sort: "desc" }, ...options }
      : options,
  };
}
const ts = (title, pos, targets, o) => panel("timeseries", title, pos, targets, o);
const stat = (title, pos, targets, o = {}) => panel("stat", title, pos, targets, { ...o, options: { reduceOptions: { calcs: ["lastNotNull"] }, colorMode: o.thresholds ? "background" : "value", graphMode: "area", ...o.options } });
const bar = (title, pos, targets, o = {}) => panel("barchart", title, pos, targets, { ...o, options: { orientation: "horizontal", showValue: "auto", legend: { showLegend: false }, ...o.options } });
const row = (title, y) => ({ id: ++id, type: "row", title, collapsed: false, gridPos: { h: 1, w: 24, x: 0, y }, panels: [] });
const g = (x, y, w, h) => ({ x, y, w, h });
const instant = (expr, legendFormat, format = "time_series") => target(expr, legendFormat, { instant: true, range: false, format });

const regionVar = {
  name: "region", label: "Region", type: "query", datasource: DS, refresh: 2, includeAll: true, multi: true,
  query: { query: "label_values(fable_players_online, region)", refId: "region" }, current: { text: "All", value: "$__all" },
};

const modeVar = {
  name: "mode", label: "Arena mode", type: "query", datasource: DS, refresh: 2, includeAll: true, multi: true,
  query: { query: "label_values(fable_arena_matches_total, mode)", refId: "mode" }, current: { text: "All", value: "$__all" },
};

function dashboard(uid, title, panels, { vars = [], links = true } = {}) {
  return {
    uid, title, tags: ["fable-online"], timezone: "utc", schemaVersion: 41, version: 1, editable: false,
    refresh: "30s", time: { from: "now-6h", to: "now" }, graphTooltip: 1,
    templating: { list: vars },
    links: links ? [{ type: "dashboards", tags: ["fable-online"], asDropdown: false, title: "Fable", includeVars: false, keepTime: true }] : [],
    panels,
  };
}

const R = 'region=~"$region"';

// ---------------------------------------------------------------------------
const overview = dashboard("fable-overview", "Fable Online: Overview", [
  stat("Heroes online", g(0, 0, 4, 4), [instant("sum(fable_players_online)")], { decimals: 0 }),
  stat("In childhood / Guild", g(4, 0, 4, 4), [instant("fable_players_in_tutorial")], { decimals: 0 }),
  stat("Unique today", g(8, 0, 4, 4), [instant("fable_players_unique_today")], { decimals: 0 }),
  stat("Regions up", g(12, 0, 4, 4), [instant("sum(fable_region_process_up)")], { decimals: 0 }),
  stat("Economy flags (24h)", g(16, 0, 4, 4), [instant("fable_economy_flagged_players")], { decimals: 0, thresholds: [{ color: "green", value: null }, { color: "orange", value: 1 }, { color: "red", value: 5 }] }),
  stat("Gold in world", g(20, 0, 4, 4), [instant("fable_gold_supply_ledger")], { unit: "short" }),
  ts("Heroes online by region", g(0, 4, 16, 9), [target(`sum by (region) (fable_players_online{${R}})`, "{{region}}")], { stack: true, min: 0 }),
  bar("Heroes per region now", g(16, 4, 8, 9), [instant(`sort_desc(sum by (region) (fable_players_online{${R}}))`, "{{region}}", "table")], {
    options: { xField: "region" },
  }),
  ts("Logins", g(0, 13, 8, 8), [target("sum by (result) (rate(fable_logins_total[5m])) * 60", "{{result}}")], { unit: "short", description: "Logins per minute by result" }),
  ts("Session length (median, p90)", g(8, 13, 8, 8), [
    target("histogram_quantile(0.5, sum by (le) (rate(fable_session_duration_seconds_bucket[1h])))", "median"),
    target("histogram_quantile(0.9, sum by (le) (rate(fable_session_duration_seconds_bucket[1h])))", "p90"),
  ], { unit: "s" }),
  ts("Guild graduations & Cullis jumps", g(16, 13, 8, 8), [
    target("sum(increase(fable_tutorial_graduations_total[1h]))", "graduations / h"),
    target("sum(increase(fable_cullis_teleports_total[1h]))", "Cullis jumps / h"),
  ]),
  ts("Chat lines per minute", g(0, 21, 12, 7), [target("sum by (channel) (rate(fable_chat_messages_total[5m])) * 60", "{{channel}}")], { stack: true }),
  ts("Reports per hour", g(12, 21, 12, 7), [target("sum by (reason) (increase(fable_chat_reports_total[1h]))", "{{reason}}")]),
], { vars: [regionVar] });

// ---------------------------------------------------------------------------
const economy = dashboard("fable-economy", "Fable Online: Economy", [
  row("Watchdog", 0),
  stat("Anomalies (1h)", g(0, 1, 6, 4), [instant("sum(increase(fable_economy_anomalies_total[1h]))")], { decimals: 0, thresholds: [{ color: "green", value: null }, { color: "red", value: 1 }] }),
  stat("Flagged heroes (24h)", g(6, 1, 6, 4), [instant("fable_economy_flagged_players")], { decimals: 0, thresholds: [{ color: "green", value: null }, { color: "orange", value: 1 }] }),
  stat("Ledger vs DB drift", g(12, 1, 6, 4), [instant("fable_gold_supply_db - fable_gold_supply_ledger")], {
    description: "Sum of all balances in the DB minus what the ledger says should exist. Anything but 0 means gold moved off-ledger (a dupe or a direct DB edit).",
    thresholds: [{ color: "green", value: null }, { color: "red", value: 1 }], options: { graphMode: "none" },
  }),
  stat("Net gold / hour", g(18, 1, 6, 4), [instant("(sum(rate(fable_gold_minted_total[1h])) - sum(rate(fable_gold_burned_total[1h]))) * 3600")], { unit: "short", description: "Minted minus burned. Positive means inflation." }),
  ts("Anomalies by rule", g(0, 5, 12, 8), [target("sum by (rule) (increase(fable_economy_anomalies_total[5m]))", "{{rule}}")], {
    stack: true, description: "Each rule is described in server/analytics/src/economy.ts. Look up the hero with economy.anomaly in the event log.",
    options: { tooltip: { mode: "multi" } }, fieldConfig: { custom: { drawStyle: "bars", fillOpacity: 80, stacking: { mode: "normal" } } },
  }),
  ts("Gold supply: ledger vs DB", g(12, 5, 12, 8), [
    target("fable_gold_supply_ledger", "ledger"), target("fable_gold_supply_db", "database"),
  ], { unit: "short" }),
  row("Faucets and sinks", 13),
  ts("Gold minted per hour, by source", g(0, 14, 12, 9), [target(`sum by (source) (rate(fable_gold_minted_total{${R}}[15m])) * 3600`, "{{source}}")], { stack: true, unit: "short" }),
  ts("Gold burned per hour, by sink", g(12, 14, 12, 9), [target(`sum by (sink) (rate(fable_gold_burned_total{${R}}[15m])) * 3600`, "{{sink}}")], { stack: true, unit: "short" }),
  bar("Minted by region (24h)", g(0, 23, 8, 9), [instant(`sort_desc(sum by (region) (increase(fable_gold_minted_total{${R}}[1d])))`, "{{region}}", "table")], { unit: "short", options: { xField: "region" } }),
  ts("Faucet / sink ratio", g(8, 23, 8, 9), [target("sum(rate(fable_gold_minted_total[1h])) / sum(rate(fable_gold_burned_total[1h]))", "minted ÷ burned")], {
    description: "Above 1 means gold piles up (inflation). Aim near 1 once the economy settles.", decimals: 2,
  }),
  ts("Transaction size p50 / p99", g(16, 23, 8, 9), [
    target("histogram_quantile(0.5, sum by (le, kind) (rate(fable_gold_transaction_size_bucket[15m])))", "p50 {{kind}}"),
    target("histogram_quantile(0.99, sum by (le, kind) (rate(fable_gold_transaction_size_bucket[15m])))", "p99 {{kind}}"),
  ], { unit: "short" }),
  row("Trade, items and phials", 32),
  ts("Gold traded between heroes per hour", g(0, 33, 8, 8), [target("sum by (kind) (rate(fable_gold_transferred_total[15m])) * 3600", "{{kind}}")], { unit: "short" }),
  ts("Items created vs destroyed per hour", g(8, 33, 8, 8), [
    target("sum by (category) (rate(fable_items_created_total[15m])) * 3600", "+ {{category}}"),
    target("-sum by (category) (rate(fable_items_destroyed_total[15m])) * 3600", "- {{category}}"),
  ]),
  ts("Resurrection Phials per day", g(16, 33, 8, 8), [
    target("sum(increase(fable_resurrection_phials_created_total[1d]))", "created"),
    target("sum(increase(fable_resurrection_phials_used_total[1d]))", "used"),
  ], { description: "Phials should stay scarce: created should track used, not run away from it." }),
], { vars: [regionVar] });

// ---------------------------------------------------------------------------
const winRate = (window) => `sum by (school) (increase(fable_arena_results_total{result="win",mode=~"$mode"}[${window}])) / sum by (school) (increase(fable_arena_results_total{result=~"win|loss",mode=~"$mode"}[${window}]))`;
const pvp = dashboard("fable-pvp", "Fable Online: PvP & Arena balance", [
  row("Arena", 0),
  stat("Matches (24h)", g(0, 1, 6, 4), [instant('sum(increase(fable_arena_matches_total{mode=~"$mode"}[1d]))')], { decimals: 0 }),
  stat("Median queue", g(6, 1, 6, 4), [instant('histogram_quantile(0.5, sum by (le) (rate(fable_arena_queue_wait_seconds_bucket{mode=~"$mode"}[1h])))')], { unit: "s" }),
  stat("Median match", g(12, 1, 6, 4), [instant('histogram_quantile(0.5, sum by (le) (rate(fable_arena_match_duration_seconds_bucket{mode=~"$mode"}[1h])))')], { unit: "s" }),
  stat("PvP-flagged heroes", g(18, 1, 6, 4), [instant("fable_pvp_flagged_players")], { decimals: 0 }),
  bar("Arena win rate by school (24h)", g(0, 5, 10, 9), [instant(winRate("1d"), "{{school}}", "table")], {
    unit: "percentunit", min: 0, max: 1, description: "Balanced is about 50% each. The alert fires above 58% with 200+ results.",
    thresholds: [{ color: "green", value: null }, { color: "orange", value: 0.55 }, { color: "red", value: 0.58 }], options: { xField: "school" },
  }),
  ts("Arena win rate by school over time", g(10, 5, 14, 9), [target(winRate("6h"), "{{school}}")], { unit: "percentunit", min: 0.3, max: 0.7 }),
  row("Kills and abilities", 14),
  panel("table", "Kills (24h): killer school down, victim school across", g(0, 15, 10, 8), [instant("sum by (killer_school, victim_school) (increase(fable_pvp_kills_total[1d]))", "", "table")], {
    decimals: 0, options: { showHeader: true },
    transformations: [{ id: "groupingToMatrix", options: { rowField: "killer_school", columnField: "victim_school", valueField: "Value" } }],
  }),
  ts("Time to kill p50 by school", g(10, 15, 14, 8), [target("histogram_quantile(0.5, sum by (le, killer_school) (rate(fable_pvp_time_to_kill_seconds_bucket[1h])))", "{{killer_school}}")], {
    unit: "s", description: "Very short time-to-kill leaves no room for counterplay.",
  }),
  bar("Damage share by ability (24h)", g(0, 23, 12, 10), [instant("sort_desc(sum by (ability) (increase(fable_pvp_damage_total[1d])) / ignoring(ability) group_left sum(increase(fable_pvp_damage_total[1d])))", "{{ability}}", "table")], {
    unit: "percentunit", options: { xField: "ability" },
  }),
  bar("How often each ability is countered (24h)", g(12, 23, 12, 10), [instant("sort(sum by (ability) (increase(fable_pvp_counters_total[1d])) / sum by (ability) (increase(fable_pvp_ability_uses_total[1d])))", "{{ability}}", "table")], {
    unit: "percentunit", description: "Share of uses that were blocked, dodged, rolled or shielded. An ability near 0% has no counterplay.", options: { xField: "ability" },
  }),
  ts("Counters by type", g(0, 33, 12, 8), [target("sum by (counter) (rate(fable_pvp_counters_total[15m])) * 60", "{{counter}}")], { stack: true }),
  ts("PvP kills per hour by venue", g(12, 33, 12, 8), [target("sum by (venue) (increase(fable_pvp_kills_total[1h]))", "{{venue}}")]),
], { vars: [modeVar] });

// ---------------------------------------------------------------------------
const health = dashboard("fable-health", "Fable Online: Server health", [
  stat("Scrape targets up", g(0, 0, 4, 4), [instant('sum(up{job="fable-server"})')], { decimals: 0 }),
  stat("Regions down", g(4, 0, 4, 4), [instant("count(fable_region_process_up == 0) or vector(0)")], { decimals: 0, thresholds: [{ color: "green", value: null }, { color: "red", value: 1 }] }),
  stat("Restarts (24h)", g(8, 0, 4, 4), [instant("sum(increase(fable_region_process_restarts_total[1d])) or vector(0)")], { decimals: 0, thresholds: [{ color: "green", value: null }, { color: "orange", value: 1 }, { color: "red", value: 10 }] }),
  stat("Open connections", g(12, 0, 4, 4), [instant("sum(fable_connections_open)")], { decimals: 0 }),
  stat("Errors / min", g(16, 0, 4, 4), [instant("sum(rate(fable_errors_total[5m])) * 60 or vector(0)")], { decimals: 1 }),
  stat("Event loop lag p99", g(20, 0, 4, 4), [instant("max(fable_nodejs_eventloop_lag_p99_seconds)")], { unit: "s" }),
  ts("Tick time p95 by region (budget 33 ms)", g(0, 4, 12, 9), [target(`histogram_quantile(0.95, sum by (le, region) (rate(fable_tick_duration_seconds_bucket{${R}}[5m])))`, "{{region}}")], {
    unit: "s", thresholds: [{ color: "green", value: null }, { color: "red", value: 0.033 }],
    fieldConfig: { custom: { thresholdsStyle: { mode: "line" } } },
  }),
  panel("state-timeline", "Region Fable.exe up", g(12, 4, 12, 9), [target(`fable_region_process_up{${R}}`, "{{region}}")], {
    thresholds: [{ color: "red", value: null }, { color: "green", value: 1 }],
    fieldConfig: { mappings: [{ type: "value", options: { 0: { text: "down" }, 1: { text: "up" } } }] }, options: { showValue: "never" },
  }),
  ts("Client round-trip p50 / p95", g(0, 13, 8, 8), [
    target(`histogram_quantile(0.5, sum by (le) (rate(fable_client_rtt_seconds_bucket{${R}}[5m])))`, "p50"),
    target(`histogram_quantile(0.95, sum by (le) (rate(fable_client_rtt_seconds_bucket{${R}}[5m])))`, "p95"),
  ], { unit: "s" }),
  ts("Network", g(8, 13, 8, 8), [target("sum by (direction) (rate(fable_net_bytes_total[5m]))", "{{direction}}")], { unit: "Bps" }),
  ts("Messages / s by type", g(16, 13, 8, 8), [target("sum by (type, direction) (rate(fable_net_messages_total[5m]))", "{{direction}} {{type}}")]),
  ts("DB query p95 by op", g(0, 21, 8, 8), [target("histogram_quantile(0.95, sum by (le, op) (rate(fable_db_query_duration_seconds_bucket[5m])))", "{{op}}")], { unit: "s" }),
  ts("Process memory", g(8, 21, 8, 8), [
    target("sum by (instance) (fable_process_resident_memory_bytes)", "RSS {{instance}}"),
  ], { unit: "bytes" }),
  ts("CPU", g(16, 21, 8, 8), [target("sum by (instance) (rate(fable_process_cpu_seconds_total[5m]))", "{{instance}}")], { unit: "percentunit" }),
  ts("Errors by kind", g(0, 29, 24, 7), [target("sum by (service, kind) (rate(fable_errors_total[5m])) * 60", "{{service}} {{kind}}")]),
], { vars: [regionVar] });

mkdirSync(OUT, { recursive: true });
for (const d of [overview, economy, pvp, health]) {
  writeFileSync(join(OUT, `${d.uid}.json`), JSON.stringify(d, null, 2) + "\n");
  console.log(`wrote ${d.uid}.json (${d.panels.length} panels)`);
}
