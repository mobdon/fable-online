import type { EventSink, GameEvent } from './events.js';
import type { Analytics } from './analytics.js';
import type { School } from './metrics.js';

/** Not one of the three schools, so the metric labels it "other". */
const UNKNOWN_SCHOOL = 'unknown' as School;

/** Sends every game event to several sinks: the raw event log and the analytics bridge. */
export class TeeSink implements EventSink {
  constructor(readonly sinks: EventSink[]) {}
  emit(e: GameEvent): void {
    for (const s of this.sinks) s.emit(e);
  }
}

/** The ledger's mint reasons, in the names the analytics allow-list uses. */
const SOURCE: Record<string, string> = {
  quest_reward: 'quest_reward',
  loot: 'monster_drop',
  vendor_sell: 'shop_sell',
  job_wage: 'job',
  gm_grant: 'gm_grant',
};
/** The ledger's sink reasons, likewise. */
const SINK: Record<string, string> = {
  vendor_buy: 'shop_buy',
  repair: 'repair',
  tattoo_haircut: 'tattoo_haircut',
  property: 'property_buy',
  arena_fee: 'arena_entry',
  death_penalty: 'death_penalty',
  crime: 'crime',
  gm_remove: 'gm_remove',
};

/** A small fixed set for the item metrics' category label, guessed from the def name. */
export function itemCategory(defId: string): string {
  if (isPhial(defId)) return 'phial';
  if (/POTION/i.test(defId)) return 'potion';
  if (/HELMET|SHIRT|GLOVES|TROUSERS|BOOTS|ARMOUR|ARMOR|_HAT|MASK/i.test(defId)) return 'armour';
  if (/SWORD|AXE|HAMMER|MACE|BOW|KATANA|CLEAVER|BLADE|DAGGER|STAFF/i.test(defId)) return 'weapon';
  return 'other';
}

const isPhial = (defId: string) => /RESURRECTION_?PHIAL/i.test(defId);
const heroOf = (wallet: string) => (wallet.startsWith('hero:') ? wallet.slice(5) : null);

export interface BridgeContext {
  /** Region a hero is in now, for the region label on gold metrics. */
  regionOf(heroId: number): string | undefined;
}

/**
 * Turns the server's own GameEvents into Analytics calls, so Prometheus and the economy
 * watchdog see every login, region change, gold movement, item, death and PvP kill
 * without each system knowing about metrics. Unknown event types are left to the event log.
 */
export class AnalyticsBridge implements EventSink {
  /** Heroes that have entered a region at least once since they connected. */
  private connected = new Set<number>();

  constructor(
    readonly analytics: Analytics,
    private ctx: BridgeContext,
  ) {}

  emit(e: GameEvent): void {
    try {
      this.handle(e);
    } catch (err) {
      // Metrics must never break the game.
      this.analytics.error('analytics', 'bridge', err);
    }
  }

  private handle(e: GameEvent): void {
    const a = this.analytics;
    switch (e.type) {
      case 'login':
        return a.login(String(e.accountId), e.ok ? 'ok' : e.reason === 'banned' ? 'banned' : 'bad_password');
      case 'player_connected':
        this.connected.add(e.heroId);
        return;
      case 'region_enter': {
        const first = this.connected.delete(e.heroId);
        return a.enterRegion(String(e.heroId), e.region, first ? 'login' : 'walk');
      }
      case 'player_disconnected':
        this.connected.delete(e.heroId);
        return a.logout(String(e.heroId));
      case 'gold': {
        const from = heroOf(e.from), to = heroOf(e.to);
        const txnId = String(e.txId);
        if (from && to) return void a.goldTransferred({ fromPlayerId: from, toPlayerId: to, amount: e.amount, kind: 'trade', txnId });
        if (to && e.from === 'system:mint')
          return void a.goldMinted({ playerId: to, amount: e.amount, source: SOURCE[e.reason] ?? e.reason, region: this.region(to), txnId });
        if (from && e.to === 'system:sink')
          return void a.goldBurned({ playerId: from, amount: e.amount, sink: SINK[e.reason] ?? e.reason, region: this.region(from), txnId });
        return;
      }
      case 'item': {
        const category = itemCategory(e.defId);
        if (e.action === 'mint' && e.toHero !== null) {
          const playerId = String(e.toHero);
          a.itemCreated({ playerId, itemDef: e.defId, category, source: 'server', count: e.quantity });
          if (isPhial(e.defId)) a.phialCreated(playerId, 'server');
        } else if (e.action === 'destroy' && e.fromHero !== null) {
          a.itemDestroyed({ playerId: String(e.fromHero), itemDef: e.defId, category, sink: 'server', count: Math.abs(e.quantity) });
        }
        return;
      }
      case 'hero_death':
        if (e.phialUsed) a.phialUsed(String(e.heroId), e.pvp ? 'open_world' : 'pve');
        return;
      case 'chat':
        if (e.event === 'chat.message') a.chat(String(e.detail.ch));
        else if (e.event === 'chat.report') a.playerReport(String(e.detail.by), String(e.detail.target ?? e.detail.targetName), 'other');
        return;
      case 'pvp_kill':
        // Schools and time-to-kill are not tracked by the combat sim yet; they show as "other".
        return a.pvpKill({
          venue: 'open_world',
          killerId: String(e.heroId),
          victimId: String(e.victimId),
          killerSchool: UNKNOWN_SCHOOL,
          victimSchool: UNKNOWN_SCHOOL,
          secondsToKill: 0,
          region: this.region(String(e.heroId)),
        });
    }
  }

  private region(heroId: string): string {
    return this.ctx.regionOf(Number(heroId)) ?? 'unknown';
  }
}
