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
import { loadSpawns } from './world/spawns.js';
import path from 'node:path';
import { TradeSessions } from './economy/trade.js';
import { RegionTable } from './world/regions.js';
import { Director } from './world/director.js';
import { stubSimFactory, type SimFactory } from './world/sim.js';
import { WorldClock } from './world/clock.js';
import { loadTownData, type TownData } from './world/town/town-data.js';
import { shopsFromTown } from './world/town/town-shops.js';
import { withTown } from './world/town/town-sim.js';

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
  clock: WorldClock;
  town: TownData;
  close(): void;
}

export function createApp(
  overrides: Partial<ServerConfig> = {},
  opts: { events?: EventSink; sim?: SimFactory; data?: GameData; ai?: (ctx: AiContext) => ChannelAi; town?: TownData; clock?: WorldClock } = {},
): App {
  const cfg = loadConfig(overrides);
  const db = new Database(cfg.dbPath);
  const events = opts.events ?? (cfg.eventsPath ? new JsonlSink(cfg.eventsPath) : new NullSink());
  const accounts = new Accounts(db, events, cfg.sessionTtlMs);
  const heroes = new Heroes(db, events, cfg.maxHeroesPerAccount);
  const data = opts.data ?? loadGameData(cfg.gameDataPath);
  const starterIds = new Set(STARTER_ITEMS.map((i) => i.id));
  const catalog = new ItemCatalog([...STARTER_ITEMS, ...catalogFromGameData(data.items, BALANCE.staffArmour).filter((i) => !starterIds.has(i.id))]);
  const economy = new Economy(db, events, catalog);
  const trades = new TradeSessions(economy);
  const progress = new Progress(db);
  const loot = new LootService(economy, data);
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
  const staffHero = (heroId: number) =>
    !!db.get("SELECT 1 FROM heroes h JOIN accounts a ON a.id = h.account_id WHERE h.id = ? AND a.role IN ('gm','admin')", heroId);
  const baseSim =
    opts.sim ??
    (cfg.combat
      ? combatSimFactory({
          data,
          spawns: loadSpawns(cfg.spawnsPath),
          events,
          economy,
          loot,
          progress,
          loadout: (heroId) => ({
            equipped: economy.inventory(heroId).filter((i) => i.location === 'equipped').map((i) => i.def_id),
            staff: staffHero(heroId),
          }),
          ...(opts.ai ? { ai: opts.ai } : {}),
        })
      : stubSimFactory);
  const sim = cfg.town ? withTown(baseSim, { data: town, clock }) : baseSim;
  const director = new Director(
    cfg,
    regions,
    sim,
    events,
    (id, region, p) => heroes.savePosition(id, region, p.x, p.y, p.z),
    clock,
  );
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
    clock,
    town,
    close() {
      director.stop();
      if (events instanceof JsonlSink) events.close();
      db.close();
    },
  };
}
