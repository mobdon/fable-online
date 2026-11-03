import type { WebSocket } from 'ws';
import type { App } from '../app.js';
import { GameError } from '../errors.js';
import type { PlayerLink } from '../world/channel.js';
import { Bucket, decode, encode, ProtocolError } from './protocol.js';

const HELLO_TIMEOUT_MS = 10_000;
/** Skip world ticks to a client whose socket is this far behind instead of buffering forever. */
const SLOW_CLIENT_BYTES = 512 * 1024;
const MAX_DROPS_PER_10S = 200;

/**
 * One player's game connection. The first frame must be `hello` carrying the one-time join
 * ticket from POST /launcher/v1/join. After that the client may only send input, chat and ping.
 */
export class GameSocket {
  private static online = new Map<number, GameSocket>();
  private heroId: number | null = null;
  private inputs = new Bucket(60, 90);
  private chats = new Bucket(1, 5);
  private drops = 0;
  private dropWindow = Date.now();
  private helloTimer: NodeJS.Timeout;

  constructor(
    private app: App,
    private ws: WebSocket,
  ) {
    this.helloTimer = setTimeout(() => this.close(4001, 'hello timeout'), HELLO_TIMEOUT_MS);
    ws.on('message', (data, isBinary) => {
      try {
        this.onMessage(isBinary ? '' : data.toString());
      } catch (e) {
        console.error(e);
        this.close(1011, 'internal error');
      }
    });
    ws.on('close', () => this.onClose());
    ws.on('error', () => this.onClose());
  }

  private onMessage(raw: string): void {
    let msg;
    try {
      msg = decode(raw);
    } catch (e) {
      if (e instanceof ProtocolError) return this.close(4002, e.message);
      throw e;
    }
    if (this.heroId === null) {
      if (msg.t !== 'hello') return this.close(4002, 'expected hello');
      return this.hello(msg.protocol, msg.token);
    }
    switch (msg.t) {
      case 'input':
        if (!this.inputs.take()) return this.dropped('input flood');
        this.app.director.input(this.heroId, msg);
        return;
      case 'chat': {
        if (!msg.text) return;
        if (!this.chats.take()) return this.send({ t: 'error', error: 'chat_rate', message: 'Slow down' });
        const ch = this.app.director.channelOf(this.heroId);
        const hero = this.app.heroes.get(this.heroId);
        ch?.broadcast(encode({ t: 'chat_out', from: this.heroId, name: hero?.name, text: msg.text }));
        return;
      }
      case 'ping':
        if (!this.inputs.take()) return this.dropped('ping flood');
        return this.send({ t: 'pong', n: msg.n });
      case 'hello':
        return this.close(4002, 'duplicate hello');
    }
  }

  private hello(protocol: number, token: string): void {
    clearTimeout(this.helloTimer);
    const { app } = this;
    try {
      if (protocol !== app.cfg.protocolVersion) throw new GameError('protocol', `Server speaks protocol ${app.cfg.protocolVersion}`, 426);
      const t = app.accounts.redeemJoinTicket(token);
      if (t.realm !== app.cfg.realmId) throw new GameError('bad_ticket', 'Ticket is for another server', 401);
      const hero = app.heroes.get(t.heroId);
      if (!hero || hero.account_id !== t.accountId) throw new GameError('bad_ticket', 'Hero not found', 401);
      // Same hero logging in twice: the newer connection wins, the old one is dropped.
      GameSocket.online.get(hero.id)?.close(4003, 'logged in elsewhere');
      const link: PlayerLink = {
        heroId: hero.id,
        name: hero.name,
        renown: hero.renown,
        lastSeq: -1,
        joinedAt: Date.now(),
        send: (frame) => {
          if (this.ws.readyState === this.ws.OPEN && this.ws.bufferedAmount < SLOW_CLIENT_BYTES) this.ws.send(frame);
        },
      };
      this.heroId = hero.id;
      GameSocket.online.set(hero.id, this);
      this.send({ t: 'welcome', player_id: hero.id, name: hero.name, phase: hero.phase, renown: hero.renown, tick_rate: app.cfg.tickRate });
      app.director.join(hero, link);
    } catch (e) {
      if (this.heroId !== null && GameSocket.online.get(this.heroId) === this) GameSocket.online.delete(this.heroId);
      this.heroId = null;
      const err = e instanceof GameError ? e : new GameError('internal', 'Join failed', 500);
      if (!(e instanceof GameError)) console.error(e);
      this.send({ t: 'error', error: err.code, message: err.message });
      this.close(4000, err.code);
    }
  }

  private dropped(reason: string): void {
    const now = Date.now();
    if (now - this.dropWindow > 10_000) {
      this.dropWindow = now;
      this.drops = 0;
    }
    if (++this.drops > MAX_DROPS_PER_10S) {
      if (this.heroId !== null) this.app.events.emit({ type: 'input_rejected', heroId: this.heroId, reason });
      this.close(4004, reason);
    }
  }

  private send(m: Record<string, unknown>): void {
    if (this.ws.readyState === this.ws.OPEN) this.ws.send(encode(m));
  }

  private close(code: number, reason: string): void {
    this.onClose();
    this.ws.close(code, reason);
  }

  private onClose(): void {
    clearTimeout(this.helloTimer);
    if (this.heroId === null) return;
    const id = this.heroId;
    this.heroId = null;
    if (GameSocket.online.get(id) === this) GameSocket.online.delete(id);
    this.app.director.leave(id);
  }
}
