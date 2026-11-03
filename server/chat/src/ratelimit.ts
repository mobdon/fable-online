// Flood control: a token bucket per player, a cooldown per busy channel,
// a repeat check, and automatic mutes for players who keep hitting the limits.
import type { TalkChannel } from "./protocol.ts";

export interface RateConfig {
  burst: number;            // messages a player can send back to back
  refillPerSec: number;     // bucket refill
  channelCooldownMs: Partial<Record<TalkChannel | "whisper", number>>;
  repeatWindowMs: number;   // same text again inside this window is refused
  strikesToAutoMute: number;
  strikeWindowMs: number;
  autoMuteMs: number;
}

export const DEFAULT_RATE: RateConfig = {
  burst: 5,
  refillPerSec: 0.5,
  channelCooldownMs: { realm: 10_000, trade: 30_000, region: 3_000 },
  repeatWindowMs: 30_000,
  strikesToAutoMute: 5,
  strikeWindowMs: 60_000,
  autoMuteMs: 5 * 60_000,
};

export type RateVerdict =
  | { ok: true }
  | { ok: false; why: "rate_limited" | "duplicate"; retryMs: number; autoMute: boolean };

export class PlayerRate {
  private tokens: number;
  private last: number;
  private lastByChannel = new Map<string, number>();
  private recent: { key: string; at: number }[] = [];
  private strikes: number[] = [];
  private cfg: RateConfig;

  constructor(cfg: RateConfig, now: number) {
    this.cfg = cfg;
    this.tokens = cfg.burst;
    this.last = now;
  }

  check(ch: TalkChannel | "whisper", text: string, now: number): RateVerdict {
    this.tokens = Math.min(this.cfg.burst, this.tokens + ((now - this.last) / 1000) * this.cfg.refillPerSec);
    this.last = now;

    const cool = this.cfg.channelCooldownMs[ch] ?? 0;
    const prev = this.lastByChannel.get(ch);
    if (prev !== undefined && now - prev < cool) return this.strike("rate_limited", cool - (now - prev), now);
    if (this.tokens < 1) return this.strike("rate_limited", Math.ceil(((1 - this.tokens) / this.cfg.refillPerSec) * 1000), now);

    const key = ch + "\u0000" + text.toLowerCase();
    this.recent = this.recent.filter((r) => now - r.at < this.cfg.repeatWindowMs);
    const dup = this.recent.find((r) => r.key === key);
    if (dup) return this.strike("duplicate", this.cfg.repeatWindowMs - (now - dup.at), now);

    this.tokens -= 1;
    this.lastByChannel.set(ch, now);
    this.recent.push({ key, at: now });
    return { ok: true };
  }

  private strike(why: "rate_limited" | "duplicate", retryMs: number, now: number): RateVerdict {
    this.strikes = this.strikes.filter((t) => now - t < this.cfg.strikeWindowMs);
    this.strikes.push(now);
    const autoMute = this.strikes.length >= this.cfg.strikesToAutoMute;
    if (autoMute) this.strikes = [];
    return { ok: false, why, retryMs: Math.max(0, Math.round(retryMs)), autoMute };
  }
}
