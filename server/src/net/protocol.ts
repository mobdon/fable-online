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
  /** `token` is the launcher's one-time join ticket, or the `resume` token from the last welcome. */
  | { t: 'hello'; protocol: number; exe_timestamp: string; token: string }
  | ({ t: 'input' } & PlayerInput)
  | { t: 'chat'; text: string }
  | { t: 'interact'; thing: string; action: string }
  | { t: 'region_ready'; region: string }
  | { t: 'ping'; n: number }
  /** The client's Fable finished a quest. The server checks the reward before paying anything. */
  | { t: 'quest_complete'; quest: string; gold: number; renown: number; items: string[] }
  /**
   * The mod reporting where the local game put its hero, used until Fable's own input is
   * mapped. A position from a client is a position a cheat can choose, so with
   * clientMovement 'checked' (the default) the server only takes moves the hero could have
   * made (src/world/movement.ts) and answers anything else with `pos_correct`.
   */
  | { t: 'hero_pos'; x: number; y: number; z: number; yaw?: number; area: number }
  /**
   * Shops. `shop` is the shop building's UID from the level file, as the client's Fable knows it;
   * the server adds the region from where it has the hero. `key` is the client's idempotency key
   * for that purchase, so a resend after a dropped connection never buys twice.
   */
  | { t: 'shop_open'; shop: string }
  | { t: 'shop_buy'; shop: string; def: string; qty: number; key: string }
  | { t: 'shop_sell'; shop: string; item: string; key: string }
  | LawRequest;

/**
 * What the mod reports to the law: what the player tried (steal, draw a weapon, answer a
 * guard), never an outcome. The server rolls, prices and records everything.
 */
export type LawRequest =
  | { t: 'law'; a: 'weapon'; on: boolean }
  | { t: 'law'; a: 'trespass'; zone: string | null }
  | { t: 'law'; a: 'report'; kind: 'lockpicking' | 'vandalism'; value: number }
  | { t: 'law'; a: 'steal'; shop: string; def: string; pickpocket: boolean; spotted: boolean; key: string }
  | { t: 'law'; a: 'answer'; choice: 'pay' | 'bribe' | 'refuse'; key: string }
  | { t: 'law'; a: 'fence'; item: string; key: string }
  | { t: 'law'; a: 'payout'; key: string }
  | { t: 'law'; a: 'warden'; key: string }
  | { t: 'law'; a: 'status' };

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
    case 'quest_complete': {
      if (typeof m.quest !== 'string' || m.quest.length > 64) throw new ProtocolError('bad quest');
      const items = Array.isArray(m.items) ? m.items.filter((i): i is string => typeof i === 'string' && i.length <= 64).slice(0, 8) : [];
      return { t: 'quest_complete', quest: m.quest, gold: isNum(m.gold) ? m.gold : -1, renown: isNum(m.renown) ? m.renown : 0, items };
    }
    case 'law':
      return decodeLaw(m);
    case 'shop_open':
      return { t: 'shop_open', shop: shopUid(m.shop) };
    case 'shop_buy': {
      if (typeof m.def !== 'string' || !/^[A-Z0-9_]{1,64}$/.test(m.def)) throw new ProtocolError('bad def');
      const qty = isNum(m.qty) ? m.qty : 1;
      if (!Number.isInteger(qty) || qty < 1 || qty > 99) throw new ProtocolError('bad qty');
      return { t: 'shop_buy', shop: shopUid(m.shop), def: m.def, qty, key: clientKey(m.key) };
    }
    case 'shop_sell':
      if (typeof m.item !== 'string' || m.item.length > 64) throw new ProtocolError('bad item');
      return { t: 'shop_sell', shop: shopUid(m.shop), item: m.item, key: clientKey(m.key) };
    default:
      throw new ProtocolError('unknown message');
  }
}

const ID = /^[A-Za-z0-9_:.\-]{1,96}$/;
const idOf = (v: unknown, what: string): string => {
  if (typeof v !== 'string' || !ID.test(v)) throw new ProtocolError(`bad ${what}`);
  return v;
};
const keyOf = (v: unknown): string => {
  if (v === undefined) return `srv-${Math.random().toString(36).slice(2)}-${Date.now()}`;
  if (typeof v !== 'string' || v.length < 8 || v.length > 128) throw new ProtocolError('bad key');
  return v;
};

function decodeLaw(m: Record<string, unknown>): LawRequest {
  switch (m.a) {
    case 'weapon':
      return { t: 'law', a: 'weapon', on: m.on === true };
    case 'trespass':
      return { t: 'law', a: 'trespass', zone: m.zone === null || m.zone === undefined ? null : idOf(m.zone, 'zone') };
    case 'report':
      if (m.kind !== 'lockpicking' && m.kind !== 'vandalism') throw new ProtocolError('bad report');
      // The value of a smashed object is the client's guess; cap it so it can't inflate a fine.
      return { t: 'law', a: 'report', kind: m.kind, value: isNum(m.value) ? Math.min(500, Math.max(0, Math.floor(m.value))) : 0 };
    case 'steal':
      return { t: 'law', a: 'steal', shop: idOf(m.shop, 'shop'), def: idOf(m.def, 'def'), pickpocket: m.pickpocket === true, spotted: m.spotted === true, key: keyOf(m.key) };
    case 'answer':
      if (m.choice !== 'pay' && m.choice !== 'bribe' && m.choice !== 'refuse') throw new ProtocolError('bad answer');
      return { t: 'law', a: 'answer', choice: m.choice, key: keyOf(m.key) };
    case 'fence':
      return { t: 'law', a: 'fence', item: idOf(m.item, 'item'), key: keyOf(m.key) };
    case 'payout':
    case 'warden':
      return { t: 'law', a: m.a, key: keyOf(m.key) };
    case 'status':
      return { t: 'law', a: 'status' };
    default:
      throw new ProtocolError('unknown law request');
  }
}

/** Level-file UIDs are decimal u64s; keep them as strings. */
function shopUid(v: unknown): string {
  if (typeof v !== 'string' || !/^[0-9A-Za-z_-]{1,24}$/.test(v)) throw new ProtocolError('bad shop');
  return v;
}

function clientKey(v: unknown): string {
  if (typeof v !== 'string' || !/^[0-9A-Za-z_-]{8,64}$/.test(v)) throw new ProtocolError('bad key');
  return v;
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
