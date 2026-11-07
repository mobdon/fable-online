import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import type { HeroLoadout } from '../combat/loadout.js';
import type { SpellId } from '../combat/rules.js';
import {
  ABILITY_SLOTS,
  DISCIPLINES,
  SPELL_OF_ABILITY,
  STAT_SLOTS,
  pay,
  type Pools,
  type XpTables,
} from './xp-tables.js';

/**
 * The server's hero experience: Fable's four pools and the levels bought with them.
 *
 * Fable keeps these in the player's own game (CTCHeroExperience and CTCInventoryAbilities),
 * where a trainer or an edited save can set anything. Online, this class is the truth and
 * the mod writes it into the local game after every change (`xp_state`).
 *
 * How experience gets in:
 *  - Kills the server runs pay straight in (grantKill).
 *  - Experience Fable gives by itself (quest rewards, monsters the server doesn't run, orbs)
 *    reaches the server as a rise in the pools the mod reports (`xp_sync`). It is taken up
 *    to a daily cap, after taking out what the server just paid for a kill (that kill's own
 *    orbs, when the stand-in dies in Fable too).
 *  - The first time a hero connects, the save's experience and bought levels are imported
 *    once, up to a cap.
 *
 * How it gets spent: when a reported level is higher than the server's, it is a purchase.
 * The server prices it from Fable's own tables (xp-tables.ts), pays it the way Fable does
 * (own pool first, then general) and keeps it, or refuses and the mod puts the level back.
 */

export interface XpState extends Pools {
  /** Changes on every update; the mod sends back the one it last wrote as `base`. */
  seq: number;
  /** Lifetime experience earned (Fable's TotalExperiencePoints). */
  total: number;
  /** Level per TrainableStatLevels slot (10). */
  stats: number[];
  /** Level per ability slot (20), 0 = not owned, 1..4. */
  abilities: number[];
}

/** What the mod read from the local game. */
export interface XpReport extends Pools {
  /** `seq` of the last xp_state the mod wrote into the game, 0 for none. */
  base: number;
  stats: number[];
  abilities: number[];
}

export interface XpPurchase {
  kind: 'stat' | 'ability';
  slot: number;
  id: string;
  level: number;
  cost: number;
}
export interface XpRefusal {
  kind: 'stat' | 'ability';
  slot: number;
  level: number;
  reason: 'unknown' | 'max_level' | 'cant_afford';
}

export interface SyncResult {
  state: XpState;
  imported: boolean;
  bought: XpPurchase[];
  refused: XpRefusal[];
  /** Experience taken from the local game this time. */
  gained: Pools;
  /** Local experience not taken (over the daily cap, or a server kill's own orbs). */
  dropped: number;
}

export interface ExperienceOptions {
  localDailyMax: number;
  importMax: number;
  echoMs: number;
}

interface Row extends Pools {
  hero_id: number;
  total: number;
  stats: string;
  abilities: string;
  imported: number;
  local_day: string | null;
  local_today: number;
}

const HISTORY = 16;
const ZERO: Pools = { general: 0, strength: 0, skill: 0, will: 0 };
const POOLS = ['general', 'strength', 'skill', 'will'] as const;

export class Experience {
  /** Sends a frame to the hero's game connection (set by the app). */
  out?: (heroId: number, msg: Record<string, unknown>) => void;
  /** Called after levels change, so the hero's live combat stats follow. */
  onLevels?: (heroId: number) => void;

  private seq: number;
  private history = new Map<number, XpState[]>();
  /** Local gain already taken against each remembered state, so a resent report pays once. */
  private paidOnBase = new WeakMap<XpState, Pools>();
  private recentKills = new Map<number, { at: number; xp: number }[]>();

  constructor(
    private db: Database,
    private events: EventSink,
    readonly tables: XpTables,
    private opts: ExperienceOptions,
    private now: () => number = Date.now,
  ) {
    // Sequence numbers from an earlier server run must not match this run's states.
    this.seq = Math.floor(now() / 1000) * 1000;
  }

  /** The hero's current experience. */
  state(heroId: number): XpState {
    const h = this.history.get(heroId);
    if (h?.length) return h[h.length - 1]!;
    return this.remember(heroId, this.fromRow(this.row(heroId)));
  }

  /** What the combat sim gives the hero: Physique hits harder, Health raises max health, and so on. */
  loadout(heroId: number): Pick<HeroLoadout, 'strength' | 'skill' | 'willLevel' | 'health' | 'spells'> {
    const s = this.state(heroId);
    const level = (id: string) => {
      for (const [slot, t] of this.tables.statBySlot) if (t.id === id) return s.stats[slot] ?? 0;
      return 0;
    };
    const spells: Partial<Record<SpellId, number>> = {};
    for (const [slot, a] of this.tables.abilityBySlot) {
      const spell = SPELL_OF_ABILITY[a.def] as SpellId | undefined;
      if (spell && (s.abilities[slot] ?? 0) > 0) spells[spell] = s.abilities[slot]!;
    }
    return { strength: level('physique'), skill: level('accuracy'), willLevel: level('will'), health: level('health'), spells };
  }

  /**
   * A kill the server ran. `xp` is the creature's experience after the combat multiplier;
   * `style` is how the hero hurt it (melee, ranged and Will damage), which decides the
   * discipline pools, as in Fable. General experience comes on top at `generalShare`.
   */
  grantKill(heroId: number, xp: number, style: { strength: number; skill: number; will: number }, generalShare: number, detail: string): void {
    if (!(xp > 0) || !this.heroExists(heroId)) return;
    const sum = style.strength + style.skill + style.will;
    const gain: Pools = { ...ZERO, general: Math.round(xp * generalShare) };
    if (sum > 0) {
      let left = Math.round(xp);
      // Largest share last, so rounding never pays more than xp in total.
      const order = [...DISCIPLINES].sort((a, b) => style[a] - style[b]);
      order.forEach((d, i) => {
        const n = i === order.length - 1 ? left : Math.round((xp * style[d]) / sum);
        gain[d] = n;
        left -= n;
      });
    } else gain.strength = Math.round(xp);
    const paid = gain.general + gain.strength + gain.skill + gain.will;
    const list = (this.recentKills.get(heroId) ?? []).filter((k) => this.now() - k.at < this.opts.echoMs);
    list.push({ at: this.now(), xp: paid });
    this.recentKills.set(heroId, list);
    this.add(heroId, gain, 'kill', detail);
  }

  /** Staff and scripts: pay experience in directly. */
  grant(heroId: number, gain: Partial<Pools>, kind: string, detail = ''): XpState {
    return this.add(heroId, { ...ZERO, ...gain }, kind, detail);
  }

  /** The mod's report of the local game: purchases, local gains, or the first import. */
  sync(heroId: number, r: XpReport): SyncResult {
    const res = this.db.tx(() => {
      const row = this.row(heroId);
      if (!row.imported) return this.importSave(heroId, r);
      const cur = this.fromRow(row);
      const base = this.history.get(heroId)?.find((s) => s.seq === r.base);
      const out: SyncResult = { state: cur, imported: false, bought: [], refused: [], gained: { ...ZERO }, dropped: 0 };

      // Purchases: a level above the server's. Priced from the server's level.
      let pools: Pools = { general: cur.general, strength: cur.strength, skill: cur.skill, will: cur.will };
      const stats = [...cur.stats];
      const abilities = [...cur.abilities];
      const buy = (kind: 'stat' | 'ability', slot: number, want: number) => {
        const have = kind === 'stat' ? stats[slot]! : abilities[slot]!;
        if (want <= have) return;
        const track = kind === 'stat' ? this.tables.statBySlot.get(slot) : this.tables.abilityBySlot.get(slot);
        const max = kind === 'stat' ? this.tables.statMax(slot) : this.tables.abilityMax(slot);
        const refuse = (reason: XpRefusal['reason']) => {
          out.refused.push({ kind, slot, level: want, reason });
          this.events.emit({ type: 'xp_buy', heroId, kind, id: track ? ('id' in track ? track.id : track.def) : `slot${slot}`, level: want, cost: 0, ok: false, reason });
        };
        if (!track) return refuse('unknown');
        if (want > max) return refuse('max_level');
        const cost = (kind === 'stat' ? this.tables.statCost(slot, have, want) : this.tables.abilityCost(slot, have, want))!;
        const after = pay(pools, track.discipline, cost);
        if (!after) return refuse('cant_afford');
        const spent = diff(pools, after);
        pools = after;
        if (kind === 'stat') stats[slot] = want;
        else abilities[slot] = want;
        const id = 'id' in track ? track.id : track.def;
        out.bought.push({ kind, slot, id, level: want, cost });
        this.log(heroId, 'buy', `${kind} ${id} ${have}->${want}`, neg(spent));
        this.events.emit({ type: 'xp_buy', heroId, kind, id, level: want, cost, ok: true });
      };
      for (let i = 0; i < STAT_SLOTS; i++) buy('stat', i, r.stats[i] ?? 0);
      for (let i = 0; i < ABILITY_SLOTS; i++) buy('ability', i, r.abilities[i] ?? 0);

      // Local gains: what the local game added since the state it was given, after taking
      // out what Fable itself charged for the levels bought since then.
      if (base) {
        let expected: Pools = { general: base.general, strength: base.strength, skill: base.skill, will: base.will };
        // Only levels the server kept: a level it refused (a trainer, an edited save) wasn't
        // paid for, so nothing of it may come back as "gained" experience.
        const kept = (want: number, now: number) => Math.min(want, now);
        for (let i = 0; i < STAT_SLOTS; i++) expected = this.fableCharge(expected, 'stat', i, base.stats[i] ?? 0, kept(r.stats[i] ?? 0, stats[i]!));
        for (let i = 0; i < ABILITY_SLOTS; i++) expected = this.fableCharge(expected, 'ability', i, base.abilities[i] ?? 0, kept(r.abilities[i] ?? 0, abilities[i]!));
        const gain: Pools = { ...ZERO };
        const paid = this.paidOnBase.get(base) ?? { ...ZERO };
        for (const p of POOLS) gain[p] = Math.max(0, Math.floor(r[p]) - expected[p] - paid[p]);
        // What the cap drops is spent too: a resend must not bring it back on a later day.
        for (const p of POOLS) paid[p] += gain[p];
        this.paidOnBase.set(base, paid);
        const taken = this.capLocal(heroId, row, gain);
        out.dropped = sumOf(gain) - sumOf(taken);
        out.gained = taken;
        for (const p of POOLS) pools[p] += taken[p];
        if (sumOf(taken) > 0) {
          this.log(heroId, 'local', 'local game', taken);
          this.events.emit({ type: 'xp_gain', heroId, source: 'local', ...taken, ...(out.dropped > 0 ? { capped: true } : {}) });
        }
      }

      const changed = out.bought.length > 0 || sumOf(out.gained) > 0;
      if (changed) this.save(heroId, pools, cur.total + sumOf(out.gained), stats, abilities);
      // Unchanged: the last state the mod was sent, so it carries a seq the mod accepts.
      out.state = changed ? this.remember(heroId, this.fromRow(this.row(heroId))) : this.state(heroId);
      return out;
    });
    if (res.bought.length || res.imported) this.onLevels?.(heroId);
    return res;
  }

  /** Sends the hero's state to their game (after a kill, a grant, or on request). */
  push(heroId: number, extra: Record<string, unknown> = {}): void {
    this.out?.(heroId, { t: 'xp_state', ...wire(this.state(heroId)), ...extra });
  }

  // ---------------------------------------------------------------- internals

  private add(heroId: number, gain: Pools, kind: string, detail: string): XpState {
    if (!this.heroExists(heroId) || sumOf(gain) <= 0) return this.state(heroId);
    const s = this.db.tx(() => {
      const cur = this.fromRow(this.row(heroId));
      const pools: Pools = { ...ZERO };
      for (const p of POOLS) pools[p] = cur[p] + Math.max(0, Math.floor(gain[p]));
      this.save(heroId, pools, cur.total + sumOf(gain), cur.stats, cur.abilities);
      this.log(heroId, kind, detail, gain);
      return this.remember(heroId, this.fromRow(this.row(heroId)));
    });
    this.events.emit({ type: 'xp_gain', heroId, source: kind, ...gain });
    this.push(heroId, { why: kind });
    return s;
  }

  /** The save's own experience, once. Levels first (in slot order), then pools, up to the cap. */
  private importSave(heroId: number, r: XpReport): SyncResult {
    let left = this.opts.importMax;
    const stats = new Array<number>(STAT_SLOTS).fill(0);
    const abilities = new Array<number>(ABILITY_SLOTS).fill(0);
    let capped = false;
    const take = (kind: 'stat' | 'ability', slot: number, want: number) => {
      const max = kind === 'stat' ? this.tables.statMax(slot) : this.tables.abilityMax(slot);
      let lvl = 0;
      for (let n = 1; n <= Math.min(want, max); n++) {
        const c = (kind === 'stat' ? this.tables.statCost(slot, n - 1, n) : this.tables.abilityCost(slot, n - 1, n)) ?? Infinity;
        if (c > left) {
          capped = true;
          break;
        }
        left -= c;
        lvl = n;
      }
      if (want > max) capped = true;
      if (kind === 'stat') stats[slot] = lvl;
      else abilities[slot] = lvl;
    };
    for (let i = 0; i < STAT_SLOTS; i++) take('stat', i, r.stats[i] ?? 0);
    for (let i = 0; i < ABILITY_SLOTS; i++) take('ability', i, r.abilities[i] ?? 0);
    const pools: Pools = { ...ZERO };
    for (const p of POOLS) {
      const want = Math.max(0, Math.floor(r[p]));
      pools[p] = Math.min(want, left);
      if (pools[p] < want) capped = true;
      left -= pools[p];
    }
    const worth = this.opts.importMax - left;
    this.save(heroId, pools, worth, stats, abilities, true);
    this.log(heroId, 'import', capped ? 'save (capped)' : 'save', pools);
    this.events.emit({ type: 'xp_gain', heroId, source: 'import', ...pools, ...(capped ? { capped: true } : {}) });
    const state = this.remember(heroId, this.fromRow(this.row(heroId)));
    return { state, imported: true, bought: [], refused: [], gained: pools, dropped: 0 };
  }

  /** What Fable itself took from the pools for levels it bought from `from` to `to`. */
  private fableCharge(p: Pools, kind: 'stat' | 'ability', slot: number, from: number, to: number): Pools {
    if (to <= from) return p;
    const track = kind === 'stat' ? this.tables.statBySlot.get(slot) : this.tables.abilityBySlot.get(slot);
    const cost = kind === 'stat' ? this.tables.statCost(slot, from, to) : this.tables.abilityCost(slot, from, to);
    if (!track || cost === null) return p;
    // Same rule as pay(), but Fable lets general go below what it had (we only need the drop).
    const own = Math.min(p[track.discipline], cost);
    return { ...p, [track.discipline]: p[track.discipline] - own, general: p.general - (cost - own) };
  }

  /** Takes out the last kills' own orbs, then holds the rest to the daily cap. */
  private capLocal(heroId: number, row: Row, gain: Pools): Pools {
    let total = sumOf(gain);
    if (total <= 0) return { ...ZERO };
    let echo = 0;
    const kills = (this.recentKills.get(heroId) ?? []).filter((k) => this.now() - k.at < this.opts.echoMs);
    for (const k of kills) {
      const use = Math.min(k.xp, total - echo);
      echo += use;
      k.xp -= use;
    }
    this.recentKills.set(heroId, kills.filter((k) => k.xp > 0));
    const day = new Date(this.now()).toISOString().slice(0, 10);
    const today = row.local_day === day ? row.local_today : 0;
    const allowed = Math.max(0, Math.min(total - echo, this.opts.localDailyMax - today));
    if (allowed <= 0) return { ...ZERO };
    // Scale every pool by the share that is kept; rounding down never pays more than allowed.
    const k = allowed / total;
    const out: Pools = { ...ZERO };
    for (const p of POOLS) out[p] = Math.floor(gain[p] * k);
    total = sumOf(out);
    this.db.run('UPDATE hero_xp SET local_day = ?, local_today = ? WHERE hero_id = ?', day, today + total, heroId);
    return out;
  }

  private heroExists(heroId: number): boolean {
    return !!this.db.get('SELECT 1 FROM heroes WHERE id = ?', heroId);
  }

  private row(heroId: number): Row {
    const r = this.db.get<Row>('SELECT * FROM hero_xp WHERE hero_id = ?', heroId);
    if (r) return r;
    if (this.heroExists(heroId)) this.db.run('INSERT OR IGNORE INTO hero_xp (hero_id, updated_at) VALUES (?, ?)', heroId, this.now());
    return { hero_id: heroId, ...ZERO, total: 0, stats: '[]', abilities: '[]', imported: 0, local_day: null, local_today: 0 };
  }

  private save(heroId: number, p: Pools, total: number, stats: number[], abilities: number[], imported = false): void {
    this.db.run(
      `UPDATE hero_xp SET general = ?, strength = ?, skill = ?, will = ?, total = ?, stats = ?, abilities = ?,
         imported = MAX(imported, ?), updated_at = ? WHERE hero_id = ?`,
      p.general,
      p.strength,
      p.skill,
      p.will,
      Math.max(0, Math.floor(total)),
      JSON.stringify(stats),
      JSON.stringify(abilities),
      imported ? 1 : 0,
      this.now(),
      heroId,
    );
  }

  private log(heroId: number, kind: string, detail: string, p: Pools): void {
    this.db.run(
      'INSERT INTO hero_xp_log (hero_id, kind, detail, general, strength, skill, will, at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
      heroId,
      kind,
      detail,
      p.general,
      p.strength,
      p.skill,
      p.will,
      this.now(),
    );
  }

  private fromRow(r: Row): XpState {
    const arr = (json: string, n: number) => {
      let a: unknown;
      try {
        a = JSON.parse(json);
      } catch {
        a = [];
      }
      const out = new Array<number>(n).fill(0);
      if (Array.isArray(a)) for (let i = 0; i < n; i++) out[i] = Math.max(0, Math.floor(Number(a[i]) || 0));
      return out;
    };
    return {
      seq: 0,
      general: r.general,
      strength: r.strength,
      skill: r.skill,
      will: r.will,
      total: r.total,
      stats: arr(r.stats, STAT_SLOTS),
      abilities: arr(r.abilities, ABILITY_SLOTS),
    };
  }

  private remember(heroId: number, s: XpState): XpState {
    s.seq = ++this.seq;
    const h = this.history.get(heroId) ?? [];
    h.push(s);
    if (h.length > HISTORY) h.splice(0, h.length - HISTORY);
    this.history.set(heroId, h);
    return s;
  }
}

/** xp_state on the wire: pools as [general, strength, skill, will]. */
export function wire(s: XpState): Record<string, unknown> {
  return { seq: s.seq, pools: [s.general, s.strength, s.skill, s.will], total: s.total, stats: s.stats, abilities: s.abilities };
}

const sumOf = (p: Pools) => p.general + p.strength + p.skill + p.will;
const diff = (a: Pools, b: Pools): Pools => ({ general: a.general - b.general, strength: a.strength - b.strength, skill: a.skill - b.skill, will: a.will - b.will });
const neg = (p: Pools): Pools => ({ general: -p.general, strength: -p.strength, skill: -p.skill, will: -p.will });

