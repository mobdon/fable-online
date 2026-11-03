import type { PlayerInput } from '../world/sim.js';

/**
 * Game socket protocol, JSON for now (one object per WebSocket text frame, field names match
 * shared/protocol/messages.proto). The codec is isolated here so it can move to protobuf
 * binary frames without touching game code.
 *
 * Client -> server: hello, input, chat, ping. That is all a client can say: intent, never
 * state. Positions, health, gold and items only ever flow server -> client.
 */
export type ClientMessage =
  | { t: 'hello'; protocol: number; exe_timestamp: string; token: string }
  | ({ t: 'input' } & PlayerInput)
  | { t: 'chat'; text: string }
  | { t: 'ping'; n: number }
  /** The client's Fable finished a quest. The server checks the reward before paying anything. */
  | { t: 'quest_complete'; quest: string; gold: number; renown: number; items: string[] };

export class ProtocolError extends Error {}

const MAX_FRAME = 2048;
export const CHAT_MAX = 200;
/** Buttons the mod is allowed to report. Unknown bits are dropped. */
export const BUTTON_MASK = 0x0000_ffff;

const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

export function decode(raw: string): ClientMessage {
  if (raw.length > MAX_FRAME) throw new ProtocolError('frame too large');
  let m: Record<string, unknown>;
  try {
    m = JSON.parse(raw);
  } catch {
    throw new ProtocolError('not json');
  }
  if (!m || typeof m !== 'object') throw new ProtocolError('not an object');
  switch (m.t) {
    case 'hello':
      if (!isNum(m.protocol) || typeof m.token !== 'string' || m.token.length > 256) throw new ProtocolError('bad hello');
      return { t: 'hello', protocol: m.protocol, exe_timestamp: String(m.exe_timestamp ?? '').slice(0, 32), token: m.token };
    case 'input': {
      if (!isNum(m.seq) || !Number.isInteger(m.seq) || m.seq < 0) throw new ProtocolError('bad seq');
      let mx = isNum(m.move_x) ? m.move_x : 0;
      let my = isNum(m.move_y) ? m.move_y : 0;
      // A stick can't be pushed past 1. Clamp the vector so nobody runs faster diagonally or by sending 50.
      const len = Math.hypot(mx, my);
      if (len > 1) {
        mx /= len;
        my /= len;
      }
      const yaw = isNum(m.camera_yaw) ? wrapAngle(m.camera_yaw) : 0;
      const buttons = isNum(m.buttons) ? (m.buttons >>> 0) & BUTTON_MASK : 0;
      return { t: 'input', seq: m.seq, moveX: mx, moveY: my, buttons, cameraYaw: yaw };
    }
    case 'chat': {
      if (typeof m.text !== 'string') throw new ProtocolError('bad chat');
      // eslint-disable-next-line no-control-regex
      const text = m.text.replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, CHAT_MAX);
      return { t: 'chat', text };
    }
    case 'ping':
      return { t: 'ping', n: isNum(m.n) ? m.n : 0 };
    case 'quest_complete': {
      if (typeof m.quest !== 'string' || m.quest.length > 64) throw new ProtocolError('bad quest');
      const items = Array.isArray(m.items) ? m.items.filter((i): i is string => typeof i === 'string' && i.length <= 64).slice(0, 8) : [];
      return { t: 'quest_complete', quest: m.quest, gold: isNum(m.gold) ? m.gold : -1, renown: isNum(m.renown) ? m.renown : 0, items };
    }
    default:
      throw new ProtocolError('unknown message');
  }
}

export function wrapAngle(a: number): number {
  const twoPi = Math.PI * 2;
  return ((((a + Math.PI) % twoPi) + twoPi) % twoPi) - Math.PI;
}

export const encode = (m: Record<string, unknown>): string => JSON.stringify(m);

/** Token bucket per connection. Inputs beyond the budget are dropped, not queued. */
export class Bucket {
  private tokens: number;
  private last = Date.now();
  constructor(
    private ratePerSec: number,
    private burst: number,
  ) {
    this.tokens = burst;
  }
  take(now = Date.now()): boolean {
    this.tokens = Math.min(this.burst, this.tokens + ((now - this.last) / 1000) * this.ratePerSec);
    this.last = now;
    if (this.tokens < 1) return false;
    this.tokens--;
    return true;
  }
}
