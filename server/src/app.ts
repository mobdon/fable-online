import { loadConfig, type ServerConfig } from './config.js';
import { Database } from './db/database.js';
import { JsonlSink, NullSink, type EventSink } from './analytics/events.js';
import { Accounts } from './accounts/accounts.js';
import { Heroes } from './heroes/heroes.js';
import { Economy } from './economy/economy.js';
import { ItemCatalog, STARTER_ITEMS, catalogFromGameData } from './economy/catalog.js';
import { LootService } from './economy/loot.js';
import { QuestRewards } from './economy/quests.js';
import { Shops } from './economy/shops.js';
import { Progress } from './heroes/progress.js';
import { loadGameData, type GameData } from './data/game-data.js';
import { BALANCE } from './combat/rules.js';
import { combatSimFactory, type ChannelAi, type AiContext } from './world/combat-sim.js';
import { loadSpawns, serverSpawnDefs } from './world/spawns.js';
import path from 'node:path';
import { TradeSessions } from './economy/trade.js';
import { RegionTable } from './world/regions.js';
import { Director } from './world/director.js';
import { stubSimFactory, type SimFactory } from './world/sim.js';
import { navSimFactory } from './world/nav-sim.js';
import { NavStore } from './world/nav/navmesh.js';
import { monsterAi } from './world/ai/combat-ai.js';
import { WorldThings } from './world/things/world-things.js';
import { WorldObjects } from './world/things/objects.js';
import { Travel, TRAVEL_RULES } from './world/travel/travel.js';
import fs from 'node:fs';
import { Law, type LawHost } from './law/law.js';
import { mergeRules } from './law/rules.js';
import type { LawServices } from './law/guards.js';
import { WorldClock } from './world/clock.js';
import { loadTownData, type TownData } from './world/town/town-data.js';
import { shopsFromTown } from './world/town/town-shops.js';
import { withTown } from './world/town/town-sim.js';
import { Scoreboard } from './scoreboard/scoreboard.js';
import { ResumeTokens } from './net/resume.js';
import { Analytics } from './analytics/analytics.js';
import { AnalyticsBridge, TeeSink } from './analytics/bridge.js';
import { JsonlFileSink, type EventSink as AnalyticsLog } from './analytics/event-log.js';
import { RealmChat } from './chat/realm-chat.js';
import { Parties } from './party/parties.js';

/** Everything the server is made of, wired together. Tests build one with ':memory:'. */
export interface App {
  cfg: ServerConfig;
  db: Database;
  events: EventSink;
  accounts: Accounts;
  heroes: Heroes;
  economy: Economy;
  trades: TradeSessions;
  data: GameData;
  progress: Progress;
  loot: LootService;
  shops: Shops;
  quests: QuestRewards;
  regions: RegionTable;
  director: Director;
  objects: WorldObjects;
  /** Cullis Gate and Guild Seal travel. */
  travel: Travel;
  law: Law;
  clock: WorldClock;
  town: TownData;
  scoreboard: Scoreboard;
  /** Lets a dropped game connection come back without a new launcher ticket. */
  resume: ResumeTokens;
  /** Prometheus metrics and the economy watchdog, fed from `events`. */
  analytics: Analytics;
  /** Realm chat and name tags; null when FO_CHAT=0. main.ts opens its port. */
  chat: RealmChat | null;
  parties: Parties;
  /** Creature defs the server spawns itself; the mod leaves the level files' copies out. Empty when it runs none. */
  spawnDefs: string[];
  /** True when the hero belongs to a gm or admin account (dev tools such as dev_spawn). */
  staffHero(heroId: number): boolean;
  close(): void;
}

export function createApp(
  overrides: Partial<ServerConfig> = {},
  opts: { events?: EventSink; sim?: SimFactory; data?: GameData; ai?: (ctx: AiContext) => ChannelAi; things?: WorldThings; town?: TownData; clock?: WorldClock } = {},
): App {
  const cfg = loadConfig(overrides);
  const db = new Database(cfg.dbPath);
  const gameLog = opts.events ?? (cfg.eventsPath ? new JsonlSink(cfg.eventsPath) : new NullSink());
  // A caller that brings its own sink (tests, scripts) owns the event output, so the analytics
  // log stays off for it; metrics are still counted.
  const analyticsLog: AnalyticsLog | undefined = !opts.events && cfg.analyticsDir ? new JsonlFileSink(cfg.analyticsDir) : undefined;
  const analytics = new Analytics({
    sink: analyticsLog ?? { write() {} },
    defaultMetrics: !opts.events,
    onAnomaly: (a) => console.warn(`economy watchdog: ${a.rule}${a.playerId ? ` hero ${a.playerId}` : ''} ${JSON.stringify(a.detail)}`),
  });
  let director: Director;
  const bridge = new AnalyticsBridge(analytics, {
    regionOf: (heroId) => director?.channelOf(heroId)?.region.name,
  });
  const events: EventSink = new TeeSink([gameLog, bridge]);
  const accounts = new Accounts(db, events, cfg.sessionTtlMs);
  const heroes = new Heroes(db, events, cfg.maxHeroesPerAccount, cfg.newHeroPhase);
  const data = opts.data ?? loadGameData(cfg.gameDataPath);
  const starterIds = new Set(STARTER_ITEMS.map((i) => i.id));
  const catalog = new ItemCatalog([...STARTER_ITEMS, ...catalogFromGameData(data.items, BALANCE.staffArmour).filter((i) => !starterIds.has(i.id))]);
  const economy = new Economy(db, events, catalog);
  const trades = new TradeSessions(economy);
  const progress = new Progress(db);
  const loot = new LootService(economy, data, { mode: cfg.lootMode, hourlyGoldCap: cfg.lootGoldPerHour });
  const clock = opts.clock ?? new WorldClock({ dayMs: cfg.dayMinutes * 60_000, weatherSlotMs: cfg.weatherMinutes * 60_000, seed: cfg.weatherSeed });
  const town = opts.town ?? loadTownData(cfg.townDataPath);
  const shops = Shops.load(economy, cfg.contentDir && path.join(cfg.contentDir, 'shops.json'), shopsFromTown(town, catalog), {
    day: () => clock.day(),
  });
  const quests = new QuestRewards(
    db,
    economy,
    events,
    QuestRewards.loadRules(cfg.contentDir && path.join(cfg.contentDir, 'quest-rewards.json')),
    QuestRewards.loadKnown(cfg.questsPath),
    data.questCards,
    { defaultMaxGold: cfg.questDefaultMaxGold, dailyGoldCap: cfg.questDailyGoldCap },
  );
  const regions = RegionTable.load(cfg.regionsPath);
  const lawFile = cfg.contentDir && path.join(cfg.contentDir, 'law.json');
  const law = new Law(db, economy, events, mergeRules(lawFile && fs.existsSync(lawFile) ? JSON.parse(fs.readFileSync(lawFile, 'utf8')) : undefined));
  const lawServices: LawServices = {
    law,
    events,
    isDay: () => !clock.isNight(),
    stealthPieces: (heroId) => economy.inventory(heroId).filter((i) => i.location === 'equipped' && /STEALTH/i.test(i.def_id)).length,
    out: {
      send: (heroId, msg) => director.sendTo(heroId, JSON.stringify(msg)),
      broadcast: (channelId, msg) => director.broadcastTo(channelId, JSON.stringify(msg)),
    },
  };
  const staffHero = (heroId: number) =>
    !!db.get("SELECT 1 FROM heroes h JOIN accounts a ON a.id = h.account_id WHERE h.id = ? AND a.role IN ('gm','admin')", heroId);
  const spawns = loadSpawns(cfg.spawnsPath);
  const spawnDefs = !opts.sim && cfg.combat ? serverSpawnDefs(spawns, data) : [];
  const baseSim =
    opts.sim ??
    (cfg.combat
      ? combatSimFactory({
          data,
          spawns,
          events,
          economy,
          loot,
          progress,
          loadout: (heroId) => ({
            equipped: economy.inventory(heroId).filter((i) => i.location === 'equipped').map((i) => i.def_id),
            staff: staffHero(heroId),
            pvp: cfg.pvpEverywhere || !!heroes.get(heroId)?.pvp_unlocked,
          }),
          spawnOptions: {
            respawnMs: cfg.monsterRespawnSec * 1000,
            generatorRespawnMs: cfg.monsterRespawnSec * 1000,
            soloDensity: cfg.monsterDensity,
          },
          ai: opts.ai ?? monsterAi(cfg.navDir ? new NavStore(cfg.navDir) : undefined),
          law: lawServices,
        })
      : cfg.navDir
        ? navSimFactory(new NavStore(cfg.navDir), lawServices)
        : stubSimFactory);
  const sim = cfg.town ? withTown(baseSim, { data: town, clock }) : baseSim;
  director = new Director(
    cfg,
    regions,
    sim,
    events,
    (id, region, p) => heroes.savePosition(id, region, p.x, p.y, p.z),
    clock,
  );
  const things = opts.things ?? WorldThings.load(cfg.thingsPath);
  const objects = new WorldObjects(things, director, economy, db, events);
  const travel = new Travel(things, director, heroes, db, events, { ...TRAVEL_RULES, barred: [...TRAVEL_RULES.barred], allOpen: cfg.gatesOpen, strictTeleports: cfg.strictTeleports });
  law.attach(lawHost(director, regions));
  trades.attach({
    send: (heroId, msg) => director.sendTo(heroId, JSON.stringify(msg)),
    where: (heroId) => {
      const ch = director.channelOf(heroId);
      const p = ch?.sim.position(heroId);
      return ch && p ? { channel: ch.id, x: p.x, y: p.y, z: p.z } : undefined;
    },
    name: (heroId) => heroes.get(heroId)?.name ?? `Hero ${heroId}`,
  });
  director.policies.push({
    tick: (dt) => trades.tick(dt),
    left: (heroId) => trades.left(heroId),
  });
  const parties = new Parties(
    events,
    {
      whereIs: (id) => {
        const ch = director.channelOf(id);
        return ch && { channel: ch.id, region: ch.region.name, shared: ch.kind === 'shared' };
      },
      canParty: (id) => heroes.get(id)?.phase === 'world',
      moveToChannel: (id, channelId) => void director.moveToChannel(id, channelId),
    },
    { maxSize: cfg.partyMaxSize },
  );
  // Party members land in the same channel, and see where the others went.
  director.companions = (id) => parties.companions(id);
  const objectHooks = director.hooks;
  director.hooks = {
    ...objectHooks,
    onEnter: (ch, link) => {
      objectHooks.onEnter?.(ch, link);
      parties.moved(link.heroId);
    },
  };
  director.policies.push({
    placement: (hero) => law.placement(hero.id),
    canEnter: (heroId, _from, to, via) => law.checkEnter(heroId, to, via),
    tick: (dt, ids) => law.tick(dt, ids),
    left: (heroId) => law.flush(heroId),
  });
  const scoreboard = new Scoreboard(db);
  const chat = cfg.chat
    ? new RealmChat(cfg, director, heroes, (event, detail) => events.emit({ type: 'chat', event, detail }))
    : null;
  // Gold in the world, from the ledger, so the supply gauges start from the truth.
  const supply = () => economy.audit().supply.circulating;
  analytics.setLedgerSupply(supply());
  let supplyAt = Date.now();
  director.policies.push({
    tick: () => {
      analytics.tick('realm', director.lastTickMs / 1000);
      const now = Date.now();
      if (now - supplyAt < SUPPLY_CHECK_MS) return;
      supplyAt = now;
      analytics.metrics.connections.set({ service: 'world' }, director.online);
      if (chat) analytics.metrics.connections.set({ service: 'chat' }, chat.hub.online);
      analytics.reportDbSupply(supply());
    },
  });
  return {
    cfg,
    db,
    events,
    accounts,
    heroes,
    economy,
    trades,
    data,
    progress,
    loot,
    shops,
    quests,
    regions,
    director,
    objects,
    travel,
    law,
    clock,
    town,
    scoreboard,
    resume: new ResumeTokens(cfg.resumeMs),
    analytics,
    chat,
    parties,
    spawnDefs,
    staffHero,
    close() {
      chat?.close();
      director.stop();
      law.flush();
      if (gameLog instanceof JsonlSink) gameLog.close();
      void analyticsLog?.close?.();
      db.close();
    },
  };
}

/** How often the ledger's gold supply is compared with the wallets (a full audit query). */
const SUPPLY_CHECK_MS = 60_000;

/**
 * The law's view of the world. Moves are queued and run after the frame, so an arrest in
 * the middle of a channel's tick never changes the channel list while it is being stepped.
 */
function lawHost(director: Director, regions: RegionTable): LawHost {
  const moves = new Map<number, { region: string; private: boolean }>();
  director.policies.push({
    tick: () => {
      for (const [heroId, m] of [...moves]) {
        moves.delete(heroId);
        if (!director.channelOf(heroId)) continue;
        try {
          director.transfer(heroId, m.region, { x: 0, y: 0, z: 0 }, 'law', {}, m.private);
        } catch (e) {
          console.error('law relocation failed', heroId, m.region, e);
        }
      }
    },
  });
  return {
    regionOf: (heroId) => moves.get(heroId)?.region ?? director.channelOf(heroId)?.region.name,
    relocate: (heroId, region, o) => void moves.set(heroId, { region, private: o.private }),
    notify: (heroId, msg) => director.sendTo(heroId, JSON.stringify(msg)),
    hasRegion: (r) => regions.has(r),
  };
}
