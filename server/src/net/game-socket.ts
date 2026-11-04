import type { WebSocket } from 'ws';
import type { App } from '../app.js';
import { GameError } from '../errors.js';
import type { PlayerLink } from '../world/channel.js';
import { ResumeTokens } from './resume.js';
import { Bucket, decode, encode, ProtocolError, type LawRequest, type ClientMessage, type PartyRequest } from './protocol.js';
import { SHOP_REACH } from '../economy/shops.js';
import { claimHeal } from '../combat/healing.js';
import { townOf } from '../world/town/town-sim.js';
import { shopIdOf } from '../world/town/townsfolk.js';

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
  private questClaims = new Bucket(0.2, 3);
  private interacts = new Bucket(4, 8);
  private lawReqs = new Bucket(5, 10);
  private shopActions = new Bucket(4, 10);
  private heals = new Bucket(2, 4);
  private partyReqs = new Bucket(2, 6);
  private partyChats = new Bucket(1, 5);
  private drops = 0;
  private dropWindow = Date.now();
  private warnedPos = false;
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
      case 'interact':
        if (!this.interacts.take()) return this.dropped('interact flood');
        return this.send(this.app.objects.interact(this.heroId, msg.thing, msg.action));
      case 'region_ready':
        this.app.director.regionReady(this.heroId, msg.region);
        return;
      case 'hero_pos':
        if (!this.inputs.take()) return this.dropped('hero_pos flood');
        return this.heroPos(this.heroId, msg);
      case 'ping':
        if (!this.inputs.take()) return this.dropped('ping flood');
        return this.send({ t: 'pong', n: msg.n });
      case 'quest_complete': {
        if (!this.questClaims.take()) return this.send({ t: 'quest_reward', ok: false, quest: msg.quest, reason: 'rate' });
        const r = this.app.quests.claim(this.heroId, { quest: msg.quest, gold: msg.gold, renown: msg.renown, items: msg.items });
        return this.send({ t: 'quest_reward', quest: msg.quest, ...r });
      }
      case 'law':
        if (!this.lawReqs.take()) return this.dropped('law flood');
        return this.law(this.heroId, msg);
      case 'shop_open':
      case 'shop_buy':
      case 'shop_sell':
        if (!this.shopActions.take()) return this.send({ t: 'shop_result', ok: false, error: 'rate', message: 'Slow down' });
        return this.shop(this.heroId, msg);
      case 'party':
        return this.party(this.heroId, msg);
      case 'heal':
        if (!this.heals.take()) return this.send({ t: 'heal_result', ok: false, key: msg.key, reason: 'rate' });
        return this.heal(this.heroId, msg.share, msg.key);
      case 'hello':
        return this.close(4002, 'duplicate hello');
    }
  }

  /**
   * The mod reporting where its Fable has the hero, until Fable's input is mapped. With
   * clientMovement 'checked' the channel accepts only moves the hero could have made, and a
   * refused one gets the server's position back so the client can snap to it.
   */
  private heroPos(heroId: number, msg: Extract<ClientMessage, { t: 'hero_pos' }>): void {
    const { app } = this;
    const mode = app.cfg.clientMovement;
    if (mode === 'off') {
      if (!this.warnedPos) {
        this.warnedPos = true;
        app.events.emit({ type: 'input_rejected', heroId, reason: 'hero_pos not accepted' });
      }
      return;
    }
    const ch = app.director.channelOf(heroId);
    if (!ch) return;
    const at = { x: msg.x, y: msg.y, z: msg.z };
    const g = ch.moves.get(heroId);
    if (g) {
      const from = g.area;
      const now = Date.now();
      let area = g.noteArea(msg.area, now);
      if (area === 'same' && msg.load) area = g.noteLoad(now);
      if (area !== 'same') app.events.emit({ type: 'area_change', heroId, channelId: ch.id, from, to: msg.area, refused: area === 'too_soon', how: msg.load ? 'load' : 'area' });
    }
    if (mode === 'trusted') {
      if (ch.placeHero(heroId, at, msg.yaw)) this.followRegion(heroId, at, false);
      return;
    }
    const before = ch.sim.position(heroId);
    const { verdict, correct } = ch.reportPosition(heroId, at, msg.yaw);
    if (verdict.ok) {
      if (verdict.anchored) {
        // Save the anchored spot now rather than at the next persist, so a crash right after
        // joining does not put the hero back where the server last had them.
        app.heroes.savePosition(heroId, ch.region.name, at.x, at.y, at.z);
        if (before)
          app.events.emit({ type: 'move_anchor', heroId, channelId: ch.id, distance: Math.round(Math.hypot(at.x - before.x, at.y - before.y, at.z - before.z)) });
      }
      // Last: this may move the hero to another channel.
      this.followRegion(heroId, at, verdict.anchored);
      return;
    }
    if (verdict.reason === 'loading' || verdict.reason === 'not_here') return;
    // One event per run of refusals, not one per packet.
    if (g && g.rejected === 1) app.events.emit({ type: 'move_rejected', heroId, channelId: ch.id, reason: verdict.reason, count: g.rejected });
    if (correct) this.send({ t: 'pos_correct', x: correct.x, y: correct.y, z: correct.z, reason: verdict.reason, area: ch.areaOf(heroId) });
  }

  private lastFollowAt = 0;

  /**
   * Keep the hero's server region in step with the region the client's Fable has loaded,
   * worked out from the position (Director.follow). Checked straight after a jump the guard
   * let through, and otherwise about once a second.
   */
  private followRegion(heroId: number, at: { x: number; y: number; z: number }, jumped: boolean): void {
    const now = Date.now();
    if (!jumped && now - this.lastFollowAt < 1000) return;
    this.lastFollowAt = now;
    this.app.director.follow(heroId, at);
  }

  private law(heroId: number, m: LawRequest): void {
    const { app } = this;
    const ch = app.director.channelOf(heroId)?.sim.law;
    try {
      switch (m.a) {
        case 'weapon':
          return ch?.setWeaponOut(heroId, m.on);
        case 'trespass':
          return ch?.setTrespass(heroId, m.zone);
        case 'report':
          return ch?.report(heroId, m.kind, m.value);
        case 'steal': {
          if (!ch) throw new GameError('no_law', 'Nothing to steal here', 409);
          const r = ch.steal(heroId, m.shop, m.def, { pickpocket: m.pickpocket, clientSpotted: m.spotted }, m.key);
          return this.send({ t: 'law_steal', spotted: r.spotted, item: r.itemId });
        }
        case 'answer':
          if (!ch) throw new GameError('no_prompt', 'Nobody is asking you anything', 409);
          return ch.answer(heroId, m.choice, m.key);
        case 'fence':
          return this.send({ t: 'law_fenced', ...app.law.fence(heroId, m.item, m.key) });
        case 'payout':
          return this.send({ t: 'law_released', ...app.law.payOut(heroId, m.key) });
        case 'warden':
          return this.send({ t: 'law_warden', ...app.law.warden(heroId, m.key) });
        case 'status':
          return app.law.pushStatus(heroId);
      }
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      this.send({ t: 'error', error: e.code, message: e.message });
    }
  }

  private party(heroId: number, m: PartyRequest): void {
    const { parties } = this.app;
    const bucket = m.op === 'say' ? this.partyChats : this.partyReqs;
    if (!bucket.take()) return this.send({ t: 'party_error', op: m.op, error: 'rate', message: 'Slow down' });
    try {
      switch (m.op) {
        case 'invite':
          return parties.invite(heroId, m.name);
        case 'accept':
          return parties.accept(heroId, m.name || undefined);
        case 'decline':
          return parties.decline(heroId, m.name || undefined);
        case 'leave':
          return parties.leave(heroId);
        case 'kick':
          return parties.kick(heroId, m.name);
        case 'lead':
          return parties.lead(heroId, m.name);
        case 'regroup':
          return parties.regroup(heroId);
        case 'say':
          return parties.say(heroId, m.text);
      }
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      this.send({ t: 'party_error', op: m.op, error: e.code, message: e.message });
    }
  }

  /**
   * The local game healed the hero. Work out what the hero could have used (Heal Life first,
   * then the healing items they carry), spend the closest match and heal by the server's own
   * amount. Health itself reaches the client in the next tick, as always.
   */
  private heal(heroId: number, share: number, clientKey: string): void {
    const { app } = this;
    const r = claimHeal(app.director.channelOf(heroId)?.sim.healing, app.economy, heroId, share, `heal:${heroId}:${clientKey}`);
    if (!r.replayed) app.events.emit({ type: 'heal', heroId, share, ok: r.ok, source: r.source, def: r.def, healed: r.healed, reason: r.reason });
    this.send({ t: 'heal_result', key: clientKey, ...r, healed: r.healed === undefined ? undefined : Math.round(r.healed) });
  }

  /**
   * The client's Fable opened a shop screen or pressed buy/sell. The server checks the hero is
   * at that shop (its own copy of the position), that the keeper is minding it, and that the
   * shop has the item, then moves gold and items in the ledger. The client only shows the result.
   */
  private shop(heroId: number, msg: Extract<ClientMessage, { t: 'shop_open' | 'shop_buy' | 'shop_sell' }>): void {
    const { app } = this;
    const ch = app.director.channelOf(heroId);
    const pos = ch?.sim.position(heroId);
    const shopId = ch ? shopIdOf(ch.region.name, msg.shop) : '';
    const key = 'key' in msg ? `shop:${heroId}:${msg.key}` : '';
    try {
      if (!ch || !pos) throw new GameError('offline', 'Not in the world');
      const at = { region: ch.region.name, pos };
      const town = townOf(ch.sim);
      if (town?.keeperOf(shopId) && !town.shopOpen(shopId)) throw new GameError('shop_closed', 'The shopkeeper is hiding');
      switch (msg.t) {
        case 'shop_open': {
          const def = app.shops.get(shopId);
          if (!def) throw new GameError('no_shop', 'No such shop', 404);
          if (def.pos && Math.hypot(pos.x - def.pos.x, pos.y - def.pos.y) > (def.reach ?? SHOP_REACH)) throw new GameError('too_far', 'You are not at that shop');
          return this.send({
            t: 'shop',
            shop: msg.shop,
            keeper: town?.keeperOf(shopId)?.id ?? null,
            gold: app.economy.balance(heroId),
            items: app.shops.listing(shopId),
            offers: app.economy
              .inventory(heroId)
              .filter((i) => i.location === 'inventory')
              .map((i) => ({ item: i.id, def: i.def_id, qty: i.quantity, gold: app.shops.offer(i.def_id) }))
              .filter((o) => o.gold !== null),
          });
        }
        case 'shop_buy': {
          const r = app.shops.buy(heroId, shopId, msg.def, msg.qty, key, at);
          app.events.emit({ type: 'shop_buy', heroId, shopId, defId: msg.def, quantity: msg.qty, price: r.price, replayed: r.replayed });
          return this.send({ t: 'shop_result', ok: true, key: msg.key, item_id: r.itemId, def: msg.def, qty: msg.qty, price: r.price, gold: app.economy.balance(heroId) });
        }
        case 'shop_sell': {
          const r = app.shops.sell(heroId, shopId, msg.item, key, at);
          app.events.emit({ type: 'shop_sell', heroId, shopId, itemId: msg.item, gold: r.gold, replayed: r.replayed });
          return this.send({ t: 'shop_result', ok: true, key: msg.key, item_id: msg.item, earned: r.gold, gold: app.economy.balance(heroId) });
        }
      }
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      return this.send({ t: 'shop_result', ok: false, key: 'key' in msg ? msg.key : undefined, shop: msg.shop, error: e.code, message: e.message });
    }
  }

  private hello(protocol: number, token: string): void {
    clearTimeout(this.helloTimer);
    const { app } = this;
    try {
      if (protocol !== app.cfg.protocolVersion) throw new GameError('protocol', `Server speaks protocol ${app.cfg.protocolVersion}`, 426);
      let t: { accountId: number; heroId: number };
      let resumed = false;
      if (ResumeTokens.looksLike(token)) {
        const r = app.resume.redeem(token);
        if (!r) throw new GameError('resume_expired', 'Reconnect window passed, start again from the launcher', 401);
        t = r;
        resumed = true;
      } else {
        const j = app.accounts.redeemJoinTicket(token);
        if (j.realm !== app.cfg.realmId) throw new GameError('bad_ticket', 'Ticket is for another server', 401);
        t = j;
      }
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
      this.send({
        t: 'welcome',
        player_id: hero.id,
        name: hero.name,
        phase: hero.phase,
        renown: hero.renown,
        tick_rate: app.cfg.tickRate,
        // Good once, after this connection ends, to come back without a new ticket.
        resume: app.resume.issue(hero.id, t.accountId),
        resume_ms: app.cfg.resumeMs,
        // Whether the server uses the mod's position reports ('off', 'checked', 'trusted').
        client_move: app.cfg.clientMovement,
        // Monster defs the server runs: the mod drops the level files' own copies of them.
        ...(app.spawnDefs.length ? { spawn_defs: app.spawnDefs } : {}),
      });
      if (resumed) app.events.emit({ type: 'player_resumed', heroId: hero.id });
      app.director.join(hero, link, resumed);
      app.law.pushStatus(hero.id);
      app.parties.connect(hero.id, hero.name, (m) => link.send(encode(m)));
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
    if (GameSocket.online.get(id) === this) {
      GameSocket.online.delete(id);
      this.app.resume.disconnected(id);
      this.app.parties.disconnect(id);
    }
    this.app.director.leave(id);
  }
}
