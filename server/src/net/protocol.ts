import type { PlayerInput } from '../world/sim.js';

/**
 * Game socket protocol, JSON for now (one object per WebSocket text frame, field names match
 * shared/protocol/messages.proto). The codec is isolated here so it can move to protobuf
 * binary frames without touching game code.
 *
 * Client -> server: hello, input, chat, interact, region_ready, ping. That is all a client
 * can say: intent, never state. Positions, health, gold and items only ever flow
 * server -> client. "interact" names a thing the hero wants to use; the server decides
 * whether they can reach it and what happens.
 */
export type ClientMessage =
  | { t: 'hello'; protocol: number; exe_timestamp: string; token: string }
  | ({ t: 'input' } & PlayerInput)
  | { t: 'chat'; text: string }
  | { t: 'interact'; thing: string; action: string }
  | { t: 'region_ready'; region: string }
  | { t: 'ping'; n: number }
  /**
   * Dev only. The mod reporting where the local game put its hero, used until Fable's
   * own input is mapped. The server throws it away unless trustClientPositions is on,
   * because a position from a client is a position a cheat can choose.
   */
  | { t: 'hero_pos'; x: number; y: number; z: number; yaw?: number; area: number };

export class ProtocolError extends Error {}

const MAX_FRAME = 2048;
export const CHAT_MAX = 200;
/** Buttons the mod is allowed to report. Unknown bits are dropped. */
export const BUTTON_MASK = 0x0000_ffff;

/** "<LevelScript>:<uid in level>" */
const THING_ID = /^[A-Za-z0-9_]{1,64}:\d{1,20}$/;
const ACTIONS = new Set(['use', 'open', 'close', 'opened']);

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
    case 'interact': {
      if (typeof m.thing !== 'string' || !THING_ID.test(m.thing)) throw new ProtocolError('bad thing');
      const action = typeof m.action === 'string' && ACTIONS.has(m.action) ? m.action : 'use';
      return { t: 'interact', thing: m.thing, action };
    }
    case 'region_ready':
      if (typeof m.region !== 'string' || m.region.length > 64) throw new ProtocolError('bad region');
      return { t: 'region_ready', region: m.region };
    case 'hero_pos': {
      if (!isNum(m.x) || !isNum(m.y) || !isNum(m.z)) throw new ProtocolError('bad hero_pos');
      const big = (v: number) => Math.abs(v) > 1e6;
      if (big(m.x) || big(m.y) || big(m.z)) throw new ProtocolError('hero_pos out of range');
      return {
        t: 'hero_pos',
        x: m.x,
        y: m.y,
        z: m.z,
        yaw: isNum(m.yaw) ? wrapAngle(m.yaw) : undefined,
        area: isNum(m.area) ? m.area >>> 0 : 0,
      };
    }
    case 'ping':
      return { t: 'ping', n: isNum(m.n) ? m.n : 0 };
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
