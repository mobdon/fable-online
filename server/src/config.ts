// All tunables in one place. Every value can be overridden with an env var.
const num = (name: string, fallback: number): number => {
  const v = process.env[name];
  if (v === undefined || v === '') return fallback;
  const n = Number(v);
  if (!Number.isFinite(n)) throw new Error(`${name} must be a number, got "${v}"`);
  return n;
};
const str = (name: string, fallback: string): string => process.env[name] || fallback;
const flag = (name: string, fallback: boolean): boolean => {
  const v = process.env[name];
  if (v === undefined || v === '') return fallback;
  return v !== '0' && v.toLowerCase() !== 'false' && v.toLowerCase() !== 'off';
};

export interface ServerConfig {
  /** HTTP + WebSocket port. */
  port: number;
  /** SQLite file for dev. ':memory:' for tests. Production moves to Postgres behind the same Store API. */
  dbPath: string;
  /** shared/defs/regions.json, generated from the player's own game files. */
  regionsPath: string;
  /**
   * shared/defs/nav, made by tools/extractor/navmesh.mjs from the player's own game files.
   * '' (the default for now) keeps every region on the kinematic StubSim.
   */
  navDir: string;
  /** shared/defs/world-things.json, made by tools/extractor/things.mjs. Missing = no doors, loot or exits. */
  thingsPath: string;
  /** JSON lines file for analytics events ('' disables). */
  eventsPath: string;
  /** Wire protocol version the client mod must speak. */
  protocolVersion: number;
  /** Oldest launcher/mod version allowed to join. */
  minClientVersion: string;
  /** This world server ("region" in the launcher API), e.g. albion-1. */
  realmId: string;
  realmName: string;
  /** Message of the day shown in the launcher. */
  motd: string;
  /** Host and port the launcher hands the mod in /join. */
  publicHost: string;
  publicPort: number;
  /** news.json and manifest-<channel>.json for the launcher. */
  contentDir: string;

  /** Simulation ticks per second for every channel. */
  tickRate: number;
  /** A channel stops receiving new players once it has this many (it still accepts party joins up to hardCap). */
  channelSoftCap: number;
  /** Absolute max players per channel. One channel = one server-mode Fable.exe. */
  channelHardCap: number;
  /** Global budget of shared channels across all regions (one Fable.exe process each). */
  maxChannels: number;
  /** Global budget of private (childhood / Guild training) instances. */
  maxPrivateInstances: number;
  /** An empty channel is shut down after this long. */
  channelIdleMs: number;
  /** Write live hero positions back to the database this often. */
  persistEveryMs: number;
  /**
   * Metres around a player whose entities are sent to them. 0 sends the whole channel.
   * Keeps a busy region's frames small, since the mod can only show what is near enough.
   */
  interestRadius: number;
  /**
   * What the server does with the mod's own hero position reports (`hero_pos`):
   * - 'off': ignore them; heroes move only from `input`.
   * - 'checked' (default): accept a report only if the hero could have got there (speed,
   *   climb and fall limits from server/src/world/movement.ts). Anything else is refused and
   *   the client is told where the server has its hero, so it snaps back.
   * - 'trusted': accept every report. Dev machines only.
   * Needed until Fable's own input is mapped and the server can move heroes from intent alone.
   * FO_TRUST_CLIENT_POS=1, the older switch, still means 'trusted'.
   */
  clientMovement: 'off' | 'checked' | 'trusted';
  /**
   * On join, accept the first position report wherever it is in the region. The client's
   * Fable loads its own save, which is not where the server last had the hero, and the mod
   * cannot yet make Fable load a given spot. Costs a fresh launcher ticket per use.
   */
  joinAnchor: boolean;
  /** How long a dropped connection may come back with its resume token instead of a new ticket. */
  resumeMs: number;
  /** The life stage new heroes start in. 'world' lets test heroes skip the private instances. */
  newHeroPhase: 'childhood' | 'guild_training' | 'world';
  /**
   * Make a hero on the first join of an account that has none, named after the account
   * ("mob" plays as "Mob"). The launcher has no hero screen yet.
   */
  autoHero: boolean;

  /** Max heroes per account. */
  maxHeroesPerAccount: number;
  /** Heroes per party, 2 to 4 (one of Fable's four co-op colours each). */
  partyMaxSize: number;
  /** Login session lifetime. */
  sessionTtlMs: number;

  /** server/data/game-data.json from `npm run extract` ('' or missing = built-in fallback). */
  gameDataPath: string;
  /** shared/defs/spawns.json from tools/extractor/spawns.mjs. */
  spawnsPath: string;
  /** shared/defs/quests.json from tools/extractor/quests.mjs (quest names in the game). */
  questsPath: string;
  /** Run combat, spawns and loot in each channel (false = movement-only stand-in). */
  combat: boolean;
  /**
   * Every hero may fight every other hero, without the Guild mission that unlocks PvP.
   * For test realms only (FO_PVP_ALL=1); off, PvP follows heroes.pvp_unlocked.
   */
  pvpEverywhere: boolean;
  /** Test servers: every Cullis Gate counts as found, so heroes from older saves can travel. */
  gatesOpen: boolean;
  /** Gold a known quest may pay when content/quest-rewards.json has no entry for it. */
  questDefaultMaxGold: number;
  /** Most quest gold one hero can be paid in 24 hours, across all quests. */
  questDailyGoldCap: number;
  /**
   * Experience the server takes from a hero's own Fable (quest rewards, kills of monsters the
   * server doesn't run, experience orbs) per UTC day. Server kills don't count towards it.
   */
  xpLocalDailyMax: number;
  /** The first time a hero connects, experience and bought levels worth up to this are taken from the save. */
  xpImportMax: number;
  /**
   * Experience the local game adds this soon after a server kill paid the same hero is taken
   * as that kill's own orbs (the stand-in dying in Fable) and not paid twice.
   */
  xpEchoMs: number;
  /**
   * Renown the server takes from a hero's own Fable the first time the mod reports it
   * (src/heroes/renown.ts). Quests run on each player's Fable, so the save already holds
   * renown the server never saw. Taken once per hero, up to this much.
   */
  renownImportMax: number;
  /** After that one import, the most renown reports may add per hero per UTC day. */
  renownDailyMax: number;
  /** 'reward': each kill pays the creature's reward value. 'tables': Fable's drop tables, gold only. */
  lootMode: 'reward' | 'tables';
  /** Most gold one hero can earn from monster kills per hour (0 = no cap). */
  lootGoldPerHour: number;
  /** Seconds before a dead monster comes back (once no hero is near its spot). */
  monsterRespawnSec: number;
  /** Generators keep this many times Fable's monster count alive with one hero; more heroes add more. */
  monsterDensity: number;

  /** server/data/town-data.json from `npm run extract-town` ('' or missing = built-in test village). */
  townDataPath: string;
  /** Run villagers and shopkeepers in regions that have villages. */
  town: boolean;
  /** Real minutes in one game day. */
  dayMinutes: number;
  /** Real minutes each weather state lasts. */
  weatherMinutes: number;
  /** Changing it reshuffles the weather. */
  weatherSeed: number;

  /** Run Realm chat (chat and name tags) inside this server. */
  chat: boolean;
  /** TCP port the chat hub listens on (NDJSON, see shared/protocol/chat.md). */
  chatPort: number;
  chatHost: string;
  /** Host and port /join tells the mod to use for chat. Empty host = publicHost. */
  chatPublicHost: string;
  chatPublicPort: number;
  /**
   * HMAC secret for chat login tokens. Empty = a new random one each boot, which is fine
   * while chat runs inside this process; set it when another process has to verify them.
   */
  chatSecret: string;
  /** How long a chat token from /join stays valid (the mod reuses it on every reconnect). */
  chatTokenTtlMs: number;
  /** First line every hero sees in Realm chat ('' = the built-in one). */
  chatMotd: string;
  /** Words the chat filter stars out. */
  chatBannedWords: string[];
  /** Chat mutes and bans ('' = kept in memory only). */
  chatModerationPath: string;

  /** Prometheus /metrics port, private interface only (0 disables it). */
  metricsPort: number;
  metricsHost: string;
  /** Directory for the analytics event log, one file per UTC day ('' disables it). */
  analyticsDir: string;
}

export function loadConfig(overrides: Partial<ServerConfig> = {}): ServerConfig {
  return {
    port: num('FO_PORT', 7777),
    dbPath: str('FO_DB', 'data/fable-online.sqlite'),
    regionsPath: str('FO_REGIONS', '../shared/defs/regions.json'),
    navDir: str('FO_NAV_DIR', ''),
    thingsPath: str('FO_THINGS', '../shared/defs/world-things.json'),
    eventsPath: str('FO_EVENTS', 'data/events.jsonl'),
    protocolVersion: 1,
    minClientVersion: str('FO_MIN_CLIENT', '0.1.0'),
    realmId: str('FO_REALM_ID', 'albion-1'),
    realmName: str('FO_REALM_NAME', 'Albion'),
    motd: str('FO_MOTD', 'Welcome to Albion'),
    publicHost: str('FO_PUBLIC_HOST', '127.0.0.1'),
    publicPort: num('FO_PUBLIC_PORT', num('FO_PORT', 7777)),
    contentDir: str('FO_CONTENT', 'content'),
    tickRate: num('FO_TICK_RATE', 20),
    channelSoftCap: num('FO_CHANNEL_SOFT_CAP', 48),
    channelHardCap: num('FO_CHANNEL_HARD_CAP', 64),
    maxChannels: num('FO_MAX_CHANNELS', 64),
    maxPrivateInstances: num('FO_MAX_PRIVATE', 400),
    channelIdleMs: num('FO_CHANNEL_IDLE_MS', 120_000),
    persistEveryMs: num('FO_PERSIST_MS', 30_000),
    interestRadius: num('FO_INTEREST_RADIUS', 80),
    clientMovement: movementMode(),
    joinAnchor: flag('FO_JOIN_ANCHOR', true),
    resumeMs: num('FO_RESUME_MS', 5 * 60_000),
    newHeroPhase: heroPhase(str('FO_NEW_HERO_PHASE', 'childhood')),
    autoHero: flag('FO_AUTO_HERO', false),
    maxHeroesPerAccount: num('FO_MAX_HEROES', 3),
    partyMaxSize: Math.min(4, Math.max(2, num('FO_PARTY_MAX', 4))),
    sessionTtlMs: num('FO_SESSION_TTL_MS', 7 * 24 * 3600_000),
    gameDataPath: str('FO_GAME_DATA', 'data/game-data.json'),
    spawnsPath: str('FO_SPAWNS', '../shared/defs/spawns.json'),
    questsPath: str('FO_QUESTS', '../shared/defs/quests.json'),
    combat: str('FO_COMBAT', '1') !== '0',
    pvpEverywhere: flag('FO_PVP_ALL', false),
    gatesOpen: flag('FO_GATES_OPEN', false),
    questDefaultMaxGold: num('FO_QUEST_DEFAULT_MAX_GOLD', 2_000),
    questDailyGoldCap: num('FO_QUEST_DAILY_GOLD_CAP', 50_000),
    xpLocalDailyMax: num('FO_XP_LOCAL_DAILY_MAX', 50_000),
    xpImportMax: num('FO_XP_IMPORT_MAX', 250_000),
    xpEchoMs: num('FO_XP_ECHO_MS', 15_000),
    renownImportMax: num('FO_RENOWN_IMPORT_MAX', 5_000),
    renownDailyMax: num('FO_RENOWN_DAILY_MAX', 5_000),
    lootMode: str('FO_LOOT_MODE', 'reward') === 'tables' ? 'tables' : 'reward',
    lootGoldPerHour: num('FO_LOOT_GOLD_PER_HOUR', 3_000),
    monsterRespawnSec: num('FO_RESPAWN_SEC', 300),
    monsterDensity: num('FO_MONSTER_DENSITY', 1.5),
    townDataPath: str('FO_TOWN_DATA', 'data/town-data.json'),
    town: str('FO_TOWN', '1') !== '0',
    dayMinutes: num('FO_DAY_MINUTES', 48),
    weatherMinutes: num('FO_WEATHER_MINUTES', 12),
    weatherSeed: num('FO_WEATHER_SEED', 1),
    chat: flag('FO_CHAT', true),
    chatPort: num('FO_CHAT_PORT', 7781),
    chatHost: str('FO_CHAT_HOST', '0.0.0.0'),
    chatPublicHost: str('FO_CHAT_PUBLIC_HOST', ''),
    chatPublicPort: num('FO_CHAT_PUBLIC_PORT', num('FO_CHAT_PORT', 7781)),
    chatSecret: str('FO_CHAT_SECRET', ''),
    chatTokenTtlMs: num('FO_CHAT_TOKEN_TTL_MS', 12 * 3600_000),
    chatMotd: str('FO_CHAT_MOTD', ''),
    chatBannedWords: str('FO_CHAT_BANNED_WORDS', '').split(',').map((w) => w.trim()).filter(Boolean),
    chatModerationPath: str('FO_CHAT_MODERATION', 'data/chat-moderation.json'),
    metricsPort: num('FO_METRICS_PORT', 9464),
    metricsHost: str('FO_METRICS_HOST', '127.0.0.1'),
    analyticsDir: str('FO_ANALYTICS_DIR', 'data/analytics'),
    ...overrides,
  };
}

function movementMode(): ServerConfig['clientMovement'] {
  if (flag('FO_TRUST_CLIENT_POS', false)) return 'trusted';
  const v = str('FO_CLIENT_MOVE', 'checked');
  if (v === 'off' || v === 'checked' || v === 'trusted') return v;
  throw new Error(`FO_CLIENT_MOVE must be off, checked or trusted, got "${v}"`);
}

function heroPhase(v: string): ServerConfig['newHeroPhase'] {
  if (v === 'childhood' || v === 'guild_training' || v === 'world') return v;
  throw new Error(`FO_NEW_HERO_PHASE must be childhood, guild_training or world, got "${v}"`);
}
