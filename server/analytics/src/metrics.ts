// Every Prometheus metric the Fable Online server exports, in one place.
//
// Rule for labels: only small, fixed sets (region, source, ability...). Never put a
// player id, account id or free text in a label: that belongs in the event log
// (events.ts), not in Prometheus.

import { Counter, Gauge, Histogram, Registry, collectDefaultMetrics } from "prom-client";

/** Combat schools. Fable's three disciplines: Strength, Skill, Will. */
export const SCHOOLS = ["melee", "ranged", "will"] as const;
export type School = (typeof SCHOOLS)[number];

/** Will spells in The Lost Chapters, plus the two weapon attack types. */
export const ABILITIES = [
  "melee_attack", "ranged_attack", "flourish",
  "lightning", "fireball", "enflame", "drain_life", "force_push", "slow_time",
  "multi_strike", "assassin_rush", "heal_life", "physical_shield", "berserk",
  "ghost_sword", "battle_charge", "turncoat", "summon", "divine_fury", "infernal_wrath",
] as const;
export type Ability = (typeof ABILITIES)[number];

/** Ways an attack can be answered. Lets us see whether each ability has counterplay. */
export const COUNTERS = ["block", "dodge", "roll", "physical_shield", "interrupt", "out_of_range"] as const;
export type Counter_ = (typeof COUNTERS)[number];

/** Every legitimate way gold can enter the world. Anything else is flagged. */
export const GOLD_SOURCES = [
  "quest_reward", "monster_drop", "chest", "shop_sell", "bounty", "arena_prize",
  "gambling_win", "rent", "job", "gm_grant",
] as const;
export type GoldSource = (typeof GOLD_SOURCES)[number];

/** Every legitimate way gold leaves the world. */
export const GOLD_SINKS = [
  "shop_buy", "tavern", "trainer", "property_buy", "gambling_loss", "death_penalty",
  "repair", "tax", "cullis_fee", "arena_entry", "gm_remove",
] as const;
export type GoldSink = (typeof GOLD_SINKS)[number];

export const PVP_VENUES = ["arena", "open_world", "duel"] as const;
export type PvpVenue = (typeof PVP_VENUES)[number];

/** Clamp a value to a known set so a bug or a hostile client can't explode label cardinality. */
export function bounded<T extends string>(value: string, allowed: readonly T[]): T | "other" {
  return (allowed as readonly string[]).includes(value) ? (value as T) : "other";
}

export function createMetrics(opts: { prefix?: string; defaultMetrics?: boolean } = {}) {
  const p = opts.prefix ?? "fable_";
  const registry = new Registry();
  if (opts.defaultMetrics ?? true) collectDefaultMetrics({ register: registry, prefix: p });
  const r = [registry];

  const m = {
    registry,

    // ---- players & sessions --------------------------------------------------
    playersOnline: new Gauge({ name: p + "players_online", help: "Heroes currently in a region", labelNames: ["region"], registers: r }),
    playersInTutorial: new Gauge({ name: p + "players_in_tutorial", help: "Heroes in their private childhood / Guild training instance", registers: r }),
    playersUniqueToday: new Gauge({ name: p + "players_unique_today", help: "Distinct accounts that logged in since 00:00 UTC", registers: r }),
    logins: new Counter({ name: p + "logins_total", help: "Login attempts at the gateway", labelNames: ["result"], registers: r }),
    sessionSeconds: new Histogram({
      name: p + "session_duration_seconds", help: "How long a play session lasted",
      buckets: [60, 300, 900, 1800, 3600, 7200, 14400, 28800], registers: r,
    }),
    regionTransfers: new Counter({ name: p + "region_transfers_total", help: "Heroes moving between regions", labelNames: ["from", "to", "via"], registers: r }),
    tutorialGraduations: new Counter({ name: p + "tutorial_graduations_total", help: "Heroes graduating from the Guild into the shared world", registers: r }),

    // ---- economy -------------------------------------------------------------
    goldMinted: new Counter({ name: p + "gold_minted_total", help: "Gold created, by source", labelNames: ["source", "region"], registers: r }),
    goldBurned: new Counter({ name: p + "gold_burned_total", help: "Gold destroyed, by sink", labelNames: ["sink", "region"], registers: r }),
    goldTransferred: new Counter({ name: p + "gold_transferred_total", help: "Gold moved between heroes (trade, mail). Net zero by construction", labelNames: ["kind"], registers: r }),
    goldSupplyLedger: new Gauge({ name: p + "gold_supply_ledger", help: "Total gold in the world according to the ledger (minted minus burned)", registers: r }),
    goldSupplyDb: new Gauge({ name: p + "gold_supply_db", help: "Total gold in the world according to a full sum of player balances in the database", registers: r }),
    goldTxSize: new Histogram({
      name: p + "gold_transaction_size", help: "Absolute size of a single gold change",
      labelNames: ["kind"], buckets: [10, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000], registers: r,
    }),
    itemsCreated: new Counter({ name: p + "items_created_total", help: "Items entering the world", labelNames: ["source", "category"], registers: r }),
    itemsDestroyed: new Counter({ name: p + "items_destroyed_total", help: "Items leaving the world", labelNames: ["sink", "category"], registers: r }),
    phialsCreated: new Counter({ name: p + "resurrection_phials_created_total", help: "Resurrection Phials entering the world", labelNames: ["source"], registers: r }),
    phialsUsed: new Counter({ name: p + "resurrection_phials_used_total", help: "Resurrection Phials consumed", labelNames: ["venue"], registers: r }),
    economyAnomalies: new Counter({ name: p + "economy_anomalies_total", help: "Ledger rule violations (possible exploits)", labelNames: ["rule"], registers: r }),
    goldEarnedPerHeroHour: new Histogram({
      name: p + "gold_earned_per_hero_hour", help: "Gold one hero earned in one finished clock hour (heroes who earned nothing are not counted)",
      buckets: [100, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 1000000], registers: r,
    }),
    goldEarnBaselineP99: new Gauge({ name: p + "gold_earn_baseline_p99", help: "99th percentile of gold per hero-hour that the watchdog has learned (0 while learning)", registers: r }),
    goldEarnOutlierLimit: new Gauge({ name: p + "gold_earn_outlier_limit", help: "Gold per hour above which earn_outlier fires (0 while learning)", registers: r }),
    economyFlaggedPlayers: new Gauge({ name: p + "economy_flagged_players", help: "Distinct heroes flagged by the economy watchdog in the last 24h", registers: r }),

    // ---- PvP & Arena -----------------------------------------------------------
    pvpKills: new Counter({ name: p + "pvp_kills_total", help: "Hero-on-hero kills", labelNames: ["venue", "killer_school", "victim_school"], registers: r }),
    pvpDamage: new Counter({ name: p + "pvp_damage_total", help: "Damage dealt to heroes by heroes", labelNames: ["venue", "ability"], registers: r }),
    pvpAbilityUses: new Counter({ name: p + "pvp_ability_uses_total", help: "Abilities used against heroes", labelNames: ["venue", "ability"], registers: r }),
    pvpCounters: new Counter({ name: p + "pvp_counters_total", help: "Abilities that were answered (blocked, dodged...)", labelNames: ["ability", "counter"], registers: r }),
    pvpTimeToKill: new Histogram({
      name: p + "pvp_time_to_kill_seconds", help: "Seconds from first hit to death in a PvP fight",
      labelNames: ["venue", "killer_school"], buckets: [1, 2, 4, 6, 10, 15, 20, 30, 45, 60, 120], registers: r,
    }),
    arenaMatches: new Counter({ name: p + "arena_matches_total", help: "Finished Arena matches", labelNames: ["mode"], registers: r }),
    arenaResults: new Counter({ name: p + "arena_results_total", help: "Arena results per hero, by their main school", labelNames: ["mode", "school", "result"], registers: r }),
    arenaMatchSeconds: new Histogram({
      name: p + "arena_match_duration_seconds", help: "Arena match length", labelNames: ["mode"],
      buckets: [30, 60, 120, 180, 300, 600, 900], registers: r,
    }),
    arenaQueueSeconds: new Histogram({
      name: p + "arena_queue_wait_seconds", help: "Time heroes wait for an Arena match", labelNames: ["mode"],
      buckets: [5, 15, 30, 60, 120, 300, 600], registers: r,
    }),
    pvpFlagged: new Gauge({ name: p + "pvp_flagged_players", help: "Online heroes with open-world PvP turned on", registers: r }),

    // ---- social ----------------------------------------------------------------
    chatMessages: new Counter({ name: p + "chat_messages_total", help: "Chat lines sent", labelNames: ["channel"], registers: r }),
    chatReports: new Counter({ name: p + "chat_reports_total", help: "Player reports", labelNames: ["reason"], registers: r }),
    cullisTeleports: new Counter({ name: p + "cullis_teleports_total", help: "Cullis Gate jumps", labelNames: ["from", "to"], registers: r }),

    // ---- server health ---------------------------------------------------------
    regionUp: new Gauge({ name: p + "region_process_up", help: "1 if the region's server-mode Fable.exe is running", labelNames: ["region"], registers: r }),
    regionRestarts: new Counter({ name: p + "region_process_restarts_total", help: "Region Fable.exe crashes / restarts", labelNames: ["region", "reason"], registers: r }),
    tickSeconds: new Histogram({
      name: p + "tick_duration_seconds", help: "Time to simulate and broadcast one world tick",
      labelNames: ["region"], buckets: [0.002, 0.005, 0.01, 0.02, 0.033, 0.05, 0.1, 0.25], registers: r,
    }),
    netMessages: new Counter({ name: p + "net_messages_total", help: "Protocol messages", labelNames: ["direction", "type"], registers: r }),
    netBytes: new Counter({ name: p + "net_bytes_total", help: "Protocol bytes", labelNames: ["direction"], registers: r }),
    connections: new Gauge({ name: p + "connections_open", help: "Open client connections", labelNames: ["service"], registers: r }),
    rttSeconds: new Histogram({
      name: p + "client_rtt_seconds", help: "Client round-trip time", labelNames: ["region"],
      buckets: [0.02, 0.05, 0.08, 0.12, 0.2, 0.3, 0.5, 1], registers: r,
    }),
    dbQuerySeconds: new Histogram({
      name: p + "db_query_duration_seconds", help: "Database query time", labelNames: ["op"],
      buckets: [0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 1], registers: r,
    }),
    errors: new Counter({ name: p + "errors_total", help: "Server errors", labelNames: ["service", "kind"], registers: r }),
  };
  return m;
}

export type Metrics = ReturnType<typeof createMetrics>;
