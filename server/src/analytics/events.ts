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
  | { type: 'tick_stats'; channels: number; players: number; tickMs: number };

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
