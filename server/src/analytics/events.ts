import fs from 'node:fs';
import path from 'node:path';

/**
 * Server-side analytics events. Every gameplay-relevant action emits one. The analytics
 * pipeline (separate work) tails the JSON-lines file or swaps in another sink.
 */
export type GameEvent =
  | { type: 'account_registered'; accountId: number }
  | { type: 'login'; accountId: number; ok: boolean; reason?: string }
  | { type: 'hero_created'; accountId: number; heroId: number }
  | { type: 'player_connected'; heroId: number; channelId: string }
  | { type: 'player_disconnected'; heroId: number; channelId: string; sessionMs: number }
  | { type: 'region_enter'; heroId: number; region: string; channelId: string; private: boolean }
  | { type: 'channel_open'; channelId: string; region: string; private: boolean }
  | { type: 'channel_close'; channelId: string; region: string; lifetimeMs: number }
  | { type: 'gold'; txId: number; kind: string; from: string; to: string; amount: number; reason: string }
  | { type: 'item'; txId: number; action: string; itemId: string; defId: string; fromHero: number | null; toHero: number | null; quantity: number }
  | { type: 'trade'; txId: number; a: number; b: number }
  | { type: 'input_rejected'; heroId: number; reason: string }
  | { type: 'move_rejected'; heroId: number; channelId: string; reason: string; count: number }
  | { type: 'move_anchor'; heroId: number; channelId: string; distance: number }
  /** The client's Fable loaded another area by itself; `refused` when it came too soon after the last. */
  | { type: 'area_change'; heroId: number; channelId: string; from: number; to: number; refused: boolean; how?: 'area' | 'load' | 'settled' }
  | { type: 'player_resumed'; heroId: number }
  /** The client's Fable loaded another region by itself and the server moved the hero there (or refused to). */
  | { type: 'region_follow'; heroId: number; from: string; to: string; channelId?: string; refused?: string }
  | { type: 'gate_unlocked'; heroId: number; gate: string; region: string }
  | { type: 'gate_travel'; heroId: number; to: string; from?: string; gate?: string | null; via?: 'seal' | 'gate' | 'back'; leader?: number; refused?: string }
  | { type: 'kill'; heroId: number; creature: string; channelId: string; experience: number; gold: number; items: number }
  | { type: 'hero_death'; heroId: number; killer: string | null; pvp: boolean; channelId: string; phialUsed: boolean }
  | { type: 'pvp_kill'; heroId: number; victimId: number; channelId: string }
  | { type: 'staff_item_blocked'; heroId: number; defId: string; action: string; actor: string }
  | { type: 'quest_reward'; heroId: number; quest: string; gold: number; items: number }
  | { type: 'quest_reward_rejected'; heroId: number; quest: string; gold: number; reason: string }
  | { type: 'interact'; heroId: number; thing: string; kind: string; ok: boolean; reason?: string }
  | { type: 'loot'; heroId: number; thing: string; txId: number; gold: number; items: number; blocked: string[] }
  | { type: 'theft'; heroId: number; thing: string; def: string; region: string }
  | { type: 'region_exit'; heroId: number; from: string; to: string; via: string; thing: string }
  | { type: 'tick_stats'; channels: number; players: number; tickMs: number }

  | { type: 'crime'; heroId: number; kind: string; town: string | null; fine: number; level: number }
  | { type: 'crime_witnessed'; heroId: number; kind: string; witness: 'guard' | 'villager'; channelId: string }
  | { type: 'fine_paid'; heroId: number; gold: number; crimes: number }
  | { type: 'bribe_paid'; heroId: number; town: string; gold: number }
  | { type: 'kicked_out'; heroId: number; town: string; minutes: number }
  | { type: 'arrest'; heroId: number; guardId: number | null; channelId: string; reason: string }
  | { type: 'sentenced'; heroId: number; prison: 'jail' | 'bargate'; minutes: number; reason: string; fines: number }
  | { type: 'released'; heroId: number; prison: 'jail' | 'bargate'; how: 'served' | 'paid' }
  | { type: 'warden_paid'; heroId: number; gold: number }
  | { type: 'escape'; heroId: number; made: boolean }
  | { type: 'steal'; heroId: number; shop: string; defId: string; value: number; spotted: boolean; chance: number }
  | { type: 'bounty_placed'; heroId: number; victim: string }
  | { type: 'shop_buy'; heroId: number; shopId: string; defId: string; quantity: number; price: number; replayed: boolean }
  | { type: 'shop_sell'; heroId: number; shopId: string; itemId: string; gold: number; replayed: boolean }
  /** The local game healed a hero: what the server matched it to and spent, or why it refused. */
  | { type: 'heal'; heroId: number; ok: boolean; share: number; source?: 'spell' | 'item'; def?: string; healed?: number; reason?: string };

export interface EventSink {
  emit(e: GameEvent): void;
}

export class NullSink implements EventSink {
  emit(): void {}
}

export class MemorySink implements EventSink {
  readonly events: (GameEvent & { at: number })[] = [];
  emit(e: GameEvent): void {
    this.events.push({ ...e, at: Date.now() });
  }
}

/** Buffered JSON-lines writer; flushes every second so the hot path never blocks on disk. */
export class JsonlSink implements EventSink {
  private buf: string[] = [];
  private timer: NodeJS.Timeout;
  constructor(private file: string) {
    fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
    this.timer = setInterval(() => this.flush(), 1000);
    this.timer.unref();
  }
  emit(e: GameEvent): void {
    this.buf.push(JSON.stringify({ at: new Date().toISOString(), ...e }));
  }
  flush(): void {
    if (this.buf.length === 0) return;
    const chunk = this.buf.join('\n') + '\n';
    this.buf = [];
    fs.appendFile(this.file, chunk, () => {});
  }
  close(): void {
    clearInterval(this.timer);
    if (this.buf.length) fs.appendFileSync(this.file, this.buf.join('\n') + '\n');
    this.buf = [];
  }
}
