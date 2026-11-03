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
import { navSimFactory } from './world/nav-sim.js';
import { NavStore } from './world/nav/navmesh.js';
import { WorldThings } from './world/things/world-things.js';
import { WorldObjects } from './world/things/objects.js';
import fs from 'node:fs';
import { Law, type LawHost } from './law/law.js';
import { mergeRules } from './law/rules.js';
import type { LawServices } from './law/guards.js';

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
  law: Law;
  close(): void;
}

export function createApp(
  overrides: Partial<ServerConfig> = {},
  opts: { events?: EventSink; sim?: SimFactory; data?: GameData; ai?: (ctx: AiContext) => ChannelAi; things?: WorldThings } = {},
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
  const shops = Shops.load(economy, cfg.contentDir && path.join(cfg.contentDir, 'shops.json'));
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
  let director: Director;
  const lawServices: LawServices = {
    law,
    events,
    stealthPieces: (heroId) => economy.inventory(heroId).filter((i) => i.location === 'equipped' && /STEALTH/i.test(i.def_id)).length,
    out: {
      send: (heroId, msg) => director.sendTo(heroId, JSON.stringify(msg)),
      broadcast: (channelId, msg) => director.broadcastTo(channelId, JSON.stringify(msg)),
    },
  };
  const staffHero = (heroId: number) =>
    !!db.get("SELECT 1 FROM heroes h JOIN accounts a ON a.id = h.account_id WHERE h.id = ? AND a.role IN ('gm','admin')", heroId);
  const sim =
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
      : cfg.navDir
        ? navSimFactory(new NavStore(cfg.navDir), lawServices)
        : stubSimFactory);
  director = new Director(cfg, regions, sim, events, (id, region, p) =>
    heroes.savePosition(id, region, p.x, p.y, p.z),
  );
  const objects = new WorldObjects(opts.things ?? WorldThings.load(cfg.thingsPath), director, economy, db, events);
  law.attach(lawHost(director, regions));
  director.policies.push({
    placement: (hero) => law.placement(hero.id),
    canEnter: (heroId, _from, to, via) => law.checkEnter(heroId, to, via),
    tick: (dt, ids) => law.tick(dt, ids),
    left: (heroId) => law.flush(heroId),
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
    law,
    close() {
      director.stop();
      law.flush();
      if (events instanceof JsonlSink) events.close();
      db.close();
    },
  };
}

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
