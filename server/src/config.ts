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
   * Dev only: accept the mod's own hero position instead of deriving it from inputs.
   * Needed until Fable's input is mapped. With this on, a client can put its hero
   * anywhere in the region, so it must never be on for a server players can reach.
   */
  trustClientPositions: boolean;

  /** Max heroes per account. */
  maxHeroesPerAccount: number;
  /** Login session lifetime. */
  sessionTtlMs: number;
}

export function loadConfig(overrides: Partial<ServerConfig> = {}): ServerConfig {
  return {
    port: num('FO_PORT', 7777),
    dbPath: str('FO_DB', 'data/fable-online.sqlite'),
    regionsPath: str('FO_REGIONS', '../shared/defs/regions.json'),
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
    trustClientPositions: flag('FO_TRUST_CLIENT_POS', false),
    maxHeroesPerAccount: num('FO_MAX_HEROES', 3),
    sessionTtlMs: num('FO_SESSION_TTL_MS', 7 * 24 * 3600_000),
    ...overrides,
  };
}
