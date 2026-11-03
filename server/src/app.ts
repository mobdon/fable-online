import { loadConfig, type ServerConfig } from './config.js';
import { Database } from './db/database.js';
import { JsonlSink, NullSink, type EventSink } from './analytics/events.js';
import { Accounts } from './accounts/accounts.js';
import { Heroes } from './heroes/heroes.js';
import { Economy } from './economy/economy.js';
import { ItemCatalog, STARTER_ITEMS } from './economy/catalog.js';
import { TradeSessions } from './economy/trade.js';
import { RegionTable } from './world/regions.js';
import { Director } from './world/director.js';
import { stubSimFactory, type SimFactory } from './world/sim.js';
import { navSimFactory } from './world/nav-sim.js';
import { NavStore } from './world/nav/navmesh.js';

/** Everything the server is made of, wired together. Tests build one with ':memory:'. */
export interface App {
  cfg: ServerConfig;
  db: Database;
  events: EventSink;
  accounts: Accounts;
  heroes: Heroes;
  economy: Economy;
  trades: TradeSessions;
  regions: RegionTable;
  director: Director;
  close(): void;
}

export function createApp(overrides: Partial<ServerConfig> = {}, opts: { events?: EventSink; sim?: SimFactory } = {}): App {
  const cfg = loadConfig(overrides);
  const db = new Database(cfg.dbPath);
  const events = opts.events ?? (cfg.eventsPath ? new JsonlSink(cfg.eventsPath) : new NullSink());
  const accounts = new Accounts(db, events, cfg.sessionTtlMs);
  const heroes = new Heroes(db, events, cfg.maxHeroesPerAccount);
  const economy = new Economy(db, events, new ItemCatalog(STARTER_ITEMS));
  const trades = new TradeSessions(economy);
  const regions = RegionTable.load(cfg.regionsPath);
  const director = new Director(cfg, regions, opts.sim ?? (cfg.navDir ? navSimFactory(new NavStore(cfg.navDir)) : stubSimFactory), events, (id, region, p) =>
    heroes.savePosition(id, region, p.x, p.y, p.z),
  );
  return {
    cfg,
    db,
    events,
    accounts,
    heroes,
    economy,
    trades,
    regions,
    director,
    close() {
      director.stop();
      if (events instanceof JsonlSink) events.close();
      db.close();
    },
  };
}
