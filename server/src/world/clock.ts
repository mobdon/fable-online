/**
 * One clock and one sky for the whole realm.
 *
 * Time of day is a pure function of wall-clock time, so every channel, every client and a
 * restarted server agree without storing anything: dayFraction = (now - epoch) / dayMs + offset.
 * Weather is also a pure function: each climate zone rolls a state for each weather slot from a
 * seeded hash of (seed, zone, slot). Two channels of the same region always show the same sky,
 * and neighbouring regions of one zone change together.
 *
 * Fable stores time of day as a fraction of a day (0 = midnight, 0.5 = noon) in its
 * CGameTimeManager; see tools/re-notes/08-world-time.md. The server sends that fraction plus the
 * day length, and the client mod writes it into the game.
 */

export type Weather = 'clear' | 'cloudy' | 'fog' | 'rain' | 'storm' | 'snow' | 'blizzard';
export type Climate = 'temperate' | 'gloom' | 'coast' | 'snow' | 'indoor';

export interface ClockOptions {
  /** Real milliseconds in one game day. */
  dayMs: number;
  /** Wall-clock time (ms) at which dayFraction is `startFraction`. */
  epoch: number;
  /** Time of day at the epoch, 0..1 (0.3 = 07:12). */
  startFraction: number;
  /** Real milliseconds a weather state lasts before the next roll. */
  weatherSlotMs: number;
  /** Changes every roll. Operators can bump it to reshuffle the weather. */
  seed: number;
  /** Clients fade from one weather state to the next over this long. */
  fadeMs: number;
}

export const DEFAULT_CLOCK: ClockOptions = {
  dayMs: 48 * 60_000,
  epoch: Date.UTC(2026, 10, 1),
  startFraction: 0.3,
  weatherSlotMs: 12 * 60_000,
  seed: 1,
  fadeMs: 30_000,
};

/** Fable's day: dawn at 06:00, dusk at 20:00 (the times its villagers keep). */
export const DAWN = 6 / 24;
export const DUSK = 20 / 24;

/** Weighted weather tables. Indoor regions have no weather, only the clock. */
const CLIMATES: Record<Climate, [Weather, number][]> = {
  temperate: [['clear', 5], ['cloudy', 3], ['rain', 2], ['fog', 1], ['storm', 0.5]],
  gloom: [['cloudy', 3], ['fog', 3], ['rain', 3], ['storm', 1]],
  coast: [['clear', 3], ['cloudy', 3], ['fog', 2], ['rain', 2], ['storm', 1]],
  snow: [['cloudy', 3], ['snow', 4], ['blizzard', 1], ['clear', 1]],
  indoor: [['clear', 1]],
};

/**
 * Which climate a region belongs to, from its level script name. Order matters: the first
 * rule that matches wins. Interiors come first so "DarkwoodChapel" counts as indoors.
 */
const CLIMATE_RULES: [RegExp, Climate][] = [
  [/Inside|Interior|Cellar|Cave|Cavern|Jail|Bedroom|House|Tavern|Bordello|Crypt|Dome|Chamber|Tomb|Library|Cells|Chapel|Temple|Mine|Tunnel|Passage|Sewer/i, 'indoor'],
  [/Snowspire|NorthernWastes|Necropolis|Wastes/i, 'snow'],
  [/HookCoast|LostBay|Coast|Shore|Beach|Island/i, 'coast'],
  [/Darkwood|Barrow|Graveyard|Witchwood|Gibbet|Hanging|Wraith|Twinblade|BanditCamp/i, 'gloom'],
];

export function climateOf(region: string): Climate {
  return CLIMATE_RULES.find(([re]) => re.test(region))?.[1] ?? 'temperate';
}

/**
 * Regions of one climate share a weather zone, except the big outdoor areas, which each get
 * their own so it can rain in Greatwood while Oakvale is dry. The zone is the region name with
 * its numeric suffix removed (Darkwood_4 and Darkwood_10 are one zone).
 */
export function zoneOf(region: string): string {
  const c = climateOf(region);
  if (c === 'indoor') return 'indoor';
  return `${c}:${region.replace(/(_v\d+|_\d+|\d+)$/i, '').replace(/^Start/, '')}`;
}

export interface SkyState {
  /** 0..1, 0 = midnight. */
  day_fraction: number;
  /** Game day number since the epoch. */
  day: number;
  /** Real ms per game day: the client advances time at 1 / day_ms per ms between updates. */
  day_ms: number;
  night: boolean;
  climate: Climate;
  weather: Weather;
  /** 0..1 strength, so two rain slots in a row can differ. */
  intensity: number;
  /** The weather that was showing before; the client fades from it over fade_ms. */
  previous: Weather;
  /** Wall-clock ms when the current weather began. */
  since: number;
  /** Wall-clock ms when it ends. */
  until: number;
  fade_ms: number;
  /** Server wall clock when this was computed, so the client can extrapolate. */
  server_time: number;
}

export const WEATHERS: readonly Weather[] = ['clear', 'cloudy', 'fog', 'rain', 'storm', 'snow', 'blizzard'];

export class WorldClock {
  readonly opts: ClockOptions;
  private now: () => number;
  /** Days added by a staff time change (dev_time). Not stored: a restart goes back to the plain clock. */
  private shiftDays = 0;
  /** Weather a staff member forced on every outdoor region until `until` (dev_weather). */
  private forced: { weather: Weather; intensity: number; since: number; until: number } | null = null;
  /** Bumped on every staff change so skyKey changes and channels resend the sky at once. */
  private version = 0;

  constructor(opts: Partial<ClockOptions> = {}, now: () => number = Date.now) {
    this.opts = { ...DEFAULT_CLOCK, ...opts };
    if (!(this.opts.dayMs >= 60_000)) throw new Error('day length must be at least a minute');
    if (!(this.opts.weatherSlotMs >= 10_000)) throw new Error('weather slot must be at least 10 s');
    this.now = now;
  }

  /** Days since the epoch, including the fraction. */
  private days(t: number): number {
    return (t - this.opts.epoch) / this.opts.dayMs + this.opts.startFraction + this.shiftDays;
  }

  /**
   * Staff tool: moves the realm's clock forward to `hour` (0..24). Always forward, so the day
   * count never goes back and shop restocks keyed on it stay correct.
   */
  setHour(hour: number, t = this.now()): void {
    if (!(hour >= 0 && hour <= 24)) throw new Error('hour must be 0..24');
    const want = (hour % 24) / 24;
    let d = want - this.dayFraction(t);
    if (d < 0) d += 1;
    this.shiftDays += d;
    this.version++;
  }

  /** Staff tool: the same weather on every outdoor region for `ms`; null weather goes back to the rolls. */
  forceWeather(weather: Weather | null, ms = 10 * 60_000, t = this.now()): void {
    this.forced = weather ? { weather, intensity: weather === 'clear' ? 0 : 1, since: t, until: t + ms } : null;
    this.version++;
  }

  dayFraction(t = this.now()): number {
    const d = this.days(t);
    return d - Math.floor(d);
  }

  /** Game days since the epoch, whole. */
  day(t = this.now()): number {
    return Math.floor(this.days(t));
  }

  /** Hour of day, 0..24. */
  hour(t = this.now()): number {
    return this.dayFraction(t) * 24;
  }

  isNight(t = this.now()): boolean {
    const f = this.dayFraction(t);
    return f < DAWN || f >= DUSK;
  }

  /** The forced weather, if one is in force at `t` for this region. */
  private forcedAt(region: string, t: number) {
    return this.forced && t >= this.forced.since && t < this.forced.until && climateOf(region) !== 'indoor' ? this.forced : null;
  }

  weatherAt(region: string, t = this.now()): { weather: Weather; intensity: number; slot: number } {
    const f = this.forcedAt(region, t);
    const rolled = this.rolledAt(region, t);
    return f ? { weather: f.weather, intensity: f.intensity, slot: rolled.slot } : rolled;
  }

  /** The weather the seeded rolls give, ignoring any staff override. */
  private rolledAt(region: string, t: number): { weather: Weather; intensity: number; slot: number } {
    const climate = climateOf(region);
    const slot = Math.floor((t - this.opts.epoch) / this.opts.weatherSlotMs);
    if (climate === 'indoor') return { weather: 'clear', intensity: 0, slot };
    const h = hash32(`${this.opts.seed}|${zoneOf(region)}|${slot}`);
    const table = CLIMATES[climate];
    const total = table.reduce((s, [, w]) => s + w, 0);
    let roll = ((h & 0xffff) / 0x10000) * total;
    let weather: Weather = table[0]![0];
    for (const [w, wt] of table) {
      if ((roll -= wt) < 0) {
        weather = w;
        break;
      }
    }
    const intensity = weather === 'clear' ? 0 : 0.4 + ((h >>> 16) / 0x10000) * 0.6;
    return { weather, intensity: Math.round(intensity * 100) / 100, slot };
  }

  sky(region: string, t = this.now()): SkyState {
    const { weather, intensity, slot } = this.weatherAt(region, t);
    let since = this.opts.epoch + slot * this.opts.weatherSlotMs;
    let until = since + this.opts.weatherSlotMs;
    let previous = this.weatherAt(region, since - 1).weather;
    const f = this.forcedAt(region, t);
    if (f) {
      since = f.since;
      until = f.until;
      previous = this.rolledAt(region, f.since - 1).weather;
    }
    return {
      day_fraction: Math.round(this.dayFraction(t) * 1e6) / 1e6,
      day: Math.floor(this.days(t)),
      day_ms: this.opts.dayMs,
      night: this.isNight(t),
      climate: climateOf(region),
      weather,
      intensity,
      previous,
      since,
      until,
      fade_ms: this.opts.fadeMs,
      server_time: t,
    };
  }

  /**
   * Changes only when something a client must react to changes (weather slot or day/night),
   * so channels can broadcast on change instead of every tick.
   */
  skyKey(region: string, t = this.now()): string {
    const forcedOn = this.forcedAt(region, t) ? 'f' : '';
    return `${this.version}|${forcedOn}|${this.weatherAt(region, t).slot}|${this.isNight(t) ? 'n' : 'd'}`;
  }
}

/** FNV-1a then a murmur finaliser: cheap, stable across Node versions, well spread. */
export function hash32(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  h ^= h >>> 16;
  h = Math.imul(h, 0x85ebca6b);
  h ^= h >>> 13;
  h = Math.imul(h, 0xc2b2ae35);
  h ^= h >>> 16;
  return h >>> 0;
}
