import { GameError } from '../errors.js';
import type { EventSink } from '../analytics/events.js';

/**
 * Parties: up to four heroes who play together.
 *
 * The server owns them. A party is made by an invite and an accept, has one leader who can
 * kick and hand the lead on, and lives only in memory (a realm restart ends every party, the
 * way most online games do it). Members keep their place for a while after a disconnect, so a
 * dropped link or a crash does not throw anyone out.
 *
 * What a party gives its members:
 * - party chat (`party_say` / `party_chat`), relayed only to members;
 * - the same channel: the Director puts a hero into the channel of a party member in the
 *   region they enter, past the soft cap and up to the hard cap (`companions`), and `regroup`
 *   moves a hero to the channel the party is in when they ended up apart;
 * - a colour each, the four colours of Fable's unused co-op mode (red, blue, yellow, green),
 *   which the mod uses for the party list and the spirit that floats over a member's stand-in.
 *
 * Heroes still in childhood or Guild training play in private instances and cannot join one.
 */
export const PARTY_COLOURS = ['red', 'blue', 'yellow', 'green'] as const;
export type PartyColour = (typeof PARTY_COLOURS)[number];

export interface PartyOptions {
  maxSize: number;
  /** An invite is good for this long. */
  inviteMs: number;
  /** A disconnected member keeps their place this long. */
  offlineGraceMs: number;
}

export const PARTY_DEFAULTS: PartyOptions = { maxSize: 4, inviteMs: 60_000, offlineGraceMs: 5 * 60_000 };

/** What the party system needs from the rest of the server. */
export interface PartyHost {
  /** Where an online hero is, or undefined while between channels or offline. */
  whereIs(heroId: number): { channel: string; region: string; shared: boolean } | undefined;
  /** May this hero be in a party at all (graduated from the Guild)? */
  canParty(heroId: number): boolean;
  /** Move a hero into another channel of the region they are in. Throws a GameError if it can't. */
  moveToChannel(heroId: number, channelId: string): void;
}

/** Messages to one client, already as plain objects (the socket encodes them). */
export type Send = (m: Record<string, unknown>) => void;

interface Member {
  heroId: number;
  name: string;
  colour: number;
  joinedAt: number;
  /** When they went offline, or null while online. */
  offlineSince: number | null;
}

interface Party {
  id: number;
  leader: number;
  members: Map<number, Member>;
  createdAt: number;
}

interface Invite {
  from: number;
  to: number;
  partyId: number | null;
  expires: number;
}

interface Online {
  name: string;
  send: Send;
}

const key = (name: string) => name.trim().toLowerCase();

export class Parties {
  readonly opts: PartyOptions;
  private parties = new Map<number, Party>();
  private memberOf = new Map<number, Party>();
  /** Pending invites by invitee, newest last. */
  private invites = new Map<number, Invite[]>();
  private online = new Map<number, Online>();
  private byName = new Map<string, number>();
  private nextId = 1;

  constructor(
    private events: EventSink,
    private host: PartyHost,
    opts: Partial<PartyOptions> = {},
    private now: () => number = Date.now,
  ) {
    this.opts = { ...PARTY_DEFAULTS, ...opts };
  }

  // ------------------------------------------------------------------ connection

  /** A hero's game connection is up. Puts them back in their party if they were in one. */
  connect(heroId: number, name: string, send: Send): void {
    this.online.set(heroId, { name, send });
    this.byName.set(key(name), heroId);
    const p = this.memberOf.get(heroId);
    if (p) {
      const m = p.members.get(heroId)!;
      const wasOffline = m.offlineSince !== null;
      m.offlineSince = null;
      if (wasOffline) this.note(p, `${m.name} is back.`, heroId);
      this.push(p);
    } else send({ t: 'party', id: null });
    for (const inv of this.invites.get(heroId) ?? []) this.sendInvite(inv);
  }

  /** A hero's game connection closed. They keep their party place for offlineGraceMs. */
  disconnect(heroId: number): void {
    const o = this.online.get(heroId);
    if (!o) return;
    this.online.delete(heroId);
    if (this.byName.get(key(o.name)) === heroId) this.byName.delete(key(o.name));
    // Invites they sent die with the connection; invites to them wait (they may resume).
    for (const [to, list] of this.invites) {
      const left = list.filter((i) => i.from !== heroId);
      if (left.length) this.invites.set(to, left);
      else this.invites.delete(to);
    }
    const p = this.memberOf.get(heroId);
    if (!p) return;
    p.members.get(heroId)!.offlineSince = this.now();
    if (this.onlineCount(p) === 0) return; // nobody left to tell; sweep() ends it later
    this.note(p, `${o.name} went offline.`, heroId);
    this.push(p);
  }

  /** A hero changed region or channel: their party's list shows where everyone is. */
  moved(heroId: number): void {
    const p = this.memberOf.get(heroId);
    if (p) this.push(p);
  }

  // ------------------------------------------------------------------ requests

  invite(from: number, toName: string): void {
    this.mustBeOnline(from);
    const to = this.byName.get(key(toName));
    if (to === undefined) throw new GameError('no_such_player', `Nobody called ${toName.trim()} is online`);
    if (to === from) throw new GameError('bad_target', 'You cannot invite yourself');
    if (!this.host.canParty(from)) throw new GameError('in_training', 'You can join a party once you have left the Guild');
    if (!this.host.canParty(to)) throw new GameError('in_training', `${this.nameOf(to)} is still training at the Guild`);
    const p = this.memberOf.get(from);
    if (p && p.leader !== from) throw new GameError('not_leader', 'Only the party leader can invite');
    if (p && p.members.size >= this.opts.maxSize) throw new GameError('party_full', `A party holds ${this.opts.maxSize} heroes`);
    const theirs = this.memberOf.get(to);
    if (theirs) throw new GameError(theirs === p ? 'already_member' : 'in_a_party', `${this.nameOf(to)} is already in a party`);
    const now = this.now();
    const list = (this.invites.get(to) ?? []).filter((i) => i.from !== from && i.expires > now);
    const inv: Invite = { from, to, partyId: p?.id ?? null, expires: now + this.opts.inviteMs };
    list.push(inv);
    this.invites.set(to, list);
    this.sendInvite(inv);
    this.tell(from, `You invited ${this.nameOf(to)} to your party.`);
    this.events.emit({ type: 'party_invite', heroId: from, targetId: to });
  }

  /** Accept an invite: the one from `fromName`, or the newest if no name is given. */
  accept(heroId: number, fromName?: string): void {
    this.mustBeOnline(heroId);
    const inv = this.takeInvite(heroId, fromName);
    if (this.memberOf.has(heroId)) throw new GameError('in_a_party', 'Leave your party first');
    if (!this.host.canParty(heroId)) throw new GameError('in_training', 'You can join a party once you have left the Guild');
    const inviterParty = this.memberOf.get(inv.from);
    // The invite was made for a party that has since changed hands, or the inviter left it.
    if (inv.partyId !== null && inviterParty?.id !== inv.partyId) throw new GameError('invite_gone', 'That party no longer exists');
    if (inviterParty && inviterParty.leader !== inv.from) throw new GameError('invite_gone', 'The inviter is no longer the party leader');
    let p = inviterParty;
    if (!p) {
      if (!this.online.has(inv.from)) throw new GameError('invite_gone', `${this.nameOf(inv.from)} is no longer online`);
      p = { id: this.nextId++, leader: inv.from, members: new Map(), createdAt: this.now() };
      this.parties.set(p.id, p);
      this.add(p, inv.from);
      this.events.emit({ type: 'party_formed', partyId: p.id, leader: inv.from });
    }
    if (p.members.size >= this.opts.maxSize) throw new GameError('party_full', 'That party is full');
    this.add(p, heroId);
    // Invites to someone who just joined a party are of no use any more.
    this.invites.delete(heroId);
    this.events.emit({ type: 'party_join', partyId: p.id, heroId, size: p.members.size });
    this.note(p, `${this.nameOf(heroId)} joined the party.`);
    this.push(p);
    // Bring the new member into the party's channel if they are in the same region already.
    this.tryRegroup(heroId, false);
  }

  decline(heroId: number, fromName?: string): void {
    const inv = this.takeInvite(heroId, fromName);
    this.tell(inv.from, `${this.nameOf(heroId)} declined your invite.`);
  }

  leave(heroId: number): void {
    const p = this.memberOf.get(heroId);
    if (!p) throw new GameError('not_in_party', 'You are not in a party');
    const name = this.nameOf(heroId);
    this.remove(p, heroId, 'left');
    this.online.get(heroId)?.send({ t: 'party', id: null });
    this.tell(heroId, 'You left the party.');
    if (this.parties.has(p.id)) this.note(p, `${name} left the party.`);
  }

  kick(heroId: number, targetName: string): void {
    const p = this.memberOf.get(heroId);
    if (!p) throw new GameError('not_in_party', 'You are not in a party');
    if (p.leader !== heroId) throw new GameError('not_leader', 'Only the party leader can remove someone');
    const target = this.memberByName(p, targetName);
    if (target === heroId) throw new GameError('bad_target', 'Use leave to leave your own party');
    const name = this.nameOf(target);
    this.remove(p, target, 'kicked');
    this.online.get(target)?.send({ t: 'party', id: null });
    this.tell(target, 'You were removed from the party.');
    if (this.parties.has(p.id)) this.note(p, `${name} was removed from the party.`);
  }

  /** Hand the lead to another member. */
  lead(heroId: number, targetName: string): void {
    const p = this.memberOf.get(heroId);
    if (!p) throw new GameError('not_in_party', 'You are not in a party');
    if (p.leader !== heroId) throw new GameError('not_leader', 'Only the party leader can hand on the lead');
    const target = this.memberByName(p, targetName);
    if (target === heroId) return;
    p.leader = target;
    this.note(p, `${this.nameOf(target)} now leads the party.`);
    this.push(p);
  }

  /** Party chat. The text is already cleaned and capped by the protocol decoder. */
  say(heroId: number, text: string): void {
    const p = this.memberOf.get(heroId);
    if (!p) throw new GameError('not_in_party', 'You are not in a party');
    if (!text) return;
    const msg = { t: 'party_chat', from: heroId, name: this.nameOf(heroId), text };
    for (const id of p.members.keys()) this.online.get(id)?.send(msg);
    this.events.emit({ type: 'party_chat', partyId: p.id, heroId, length: text.length });
  }

  /** Move to the channel where most of the party is, in the region this hero is in. */
  regroup(heroId: number): void {
    if (!this.memberOf.has(heroId)) throw new GameError('not_in_party', 'You are not in a party');
    this.tryRegroup(heroId, true);
  }

  /** Online party members other than this hero (the Director's placement preference). */
  companions(heroId: number): number[] {
    const p = this.memberOf.get(heroId);
    if (!p) return [];
    return [...p.members.keys()].filter((id) => id !== heroId && this.online.has(id));
  }

  partyOf(heroId: number): { id: number; leader: number; members: number[] } | undefined {
    const p = this.memberOf.get(heroId);
    return p && { id: p.id, leader: p.leader, members: [...p.members.keys()] };
  }

  get count(): number {
    return this.parties.size;
  }

  /** Drops expired invites and members offline for longer than the grace. Call every few seconds. */
  sweep(): void {
    const now = this.now();
    for (const [to, list] of this.invites) {
      const left = list.filter((i) => i.expires > now);
      for (const i of list) if (i.expires <= now) this.tell(i.from, `Your invite to ${this.nameOf(to)} ran out.`);
      if (left.length) this.invites.set(to, left);
      else this.invites.delete(to);
    }
    for (const p of [...this.parties.values()]) {
      for (const m of [...p.members.values()]) {
        if (m.offlineSince === null || now - m.offlineSince < this.opts.offlineGraceMs) continue;
        this.remove(p, m.heroId, 'timeout');
        if (this.parties.has(p.id)) this.note(p, `${m.name} was offline too long and left the party.`);
      }
    }
  }

  // ------------------------------------------------------------------ internals

  private add(p: Party, heroId: number): void {
    const used = new Set([...p.members.values()].map((m) => m.colour));
    let colour = 0;
    while (used.has(colour)) colour++;
    p.members.set(heroId, { heroId, name: this.nameOf(heroId), colour, joinedAt: this.now(), offlineSince: null });
    this.memberOf.set(heroId, p);
  }

  /** Takes a member out; hands on the lead; ends a party left with one hero. */
  private remove(p: Party, heroId: number, why: 'left' | 'kicked' | 'timeout' | 'disband'): void {
    p.members.delete(heroId);
    this.memberOf.delete(heroId);
    this.events.emit({ type: 'party_leave', partyId: p.id, heroId, reason: why, size: p.members.size });
    if (p.members.size <= 1) return this.disband(p);
    if (p.leader === heroId) {
      // The member who has been in longest and is online takes over; failing that, anyone.
      const next = [...p.members.values()].sort((a, b) => Number(a.offlineSince !== null) - Number(b.offlineSince !== null) || a.joinedAt - b.joinedAt)[0]!;
      p.leader = next.heroId;
      this.note(p, `${next.name} now leads the party.`);
    }
    this.push(p);
  }

  private disband(p: Party): void {
    for (const id of [...p.members.keys()]) {
      p.members.delete(id);
      this.memberOf.delete(id);
      this.online.get(id)?.send({ t: 'party', id: null });
      this.tell(id, 'Your party has broken up.');
    }
    this.parties.delete(p.id);
    // Invites out from this party are void.
    for (const [to, list] of this.invites) {
      const left = list.filter((i) => i.partyId !== p.id);
      if (left.length) this.invites.set(to, left);
      else this.invites.delete(to);
    }
    this.events.emit({ type: 'party_disband', partyId: p.id, lifetimeMs: this.now() - p.createdAt });
  }

  private tryRegroup(heroId: number, asked: boolean): void {
    const here = this.host.whereIs(heroId);
    if (!here) {
      if (asked) throw new GameError('busy', 'Wait until you have arrived');
      return;
    }
    if (!here.shared) {
      if (asked) throw new GameError('private', 'You are in a place of your own');
      return;
    }
    // Count party members per channel of this region, and go to the busiest one.
    const counts = new Map<string, number>();
    for (const id of this.companions(heroId)) {
      const w = this.host.whereIs(id);
      if (w && w.shared && w.region === here.region) counts.set(w.channel, (counts.get(w.channel) ?? 0) + 1);
    }
    const mine = counts.get(here.channel) ?? 0;
    let best: string | undefined;
    let bestN = mine;
    for (const [ch, n] of counts) if (n > bestN) [best, bestN] = [ch, n];
    if (!best) {
      if (asked) this.tell(heroId, counts.size ? 'You are already with your party.' : 'Nobody from your party is in this region.');
      return;
    }
    try {
      this.host.moveToChannel(heroId, best);
      this.tell(heroId, 'You crossed over to where your party is.');
    } catch (e) {
      if (!(e instanceof GameError)) throw e;
      if (asked) throw e;
    }
  }

  private takeInvite(heroId: number, fromName?: string): Invite {
    const now = this.now();
    const list = (this.invites.get(heroId) ?? []).filter((i) => i.expires > now);
    const from = fromName?.trim() ? this.byName.get(key(fromName)) : undefined;
    if (fromName?.trim() && from === undefined) throw new GameError('no_invite', `No invite from ${fromName.trim()}`);
    const idx = from === undefined ? list.length - 1 : list.findIndex((i) => i.from === from);
    if (idx < 0) throw new GameError('no_invite', fromName?.trim() ? `No invite from ${fromName.trim()}` : 'You have no party invite');
    const inv = list.splice(idx, 1)[0]!;
    if (list.length) this.invites.set(heroId, list);
    else this.invites.delete(heroId);
    return inv;
  }

  private sendInvite(inv: Invite): void {
    const p = this.memberOf.get(inv.from);
    this.online.get(inv.to)?.send({
      t: 'party_invite',
      from: inv.from,
      name: this.nameOf(inv.from),
      size: p?.members.size ?? 1,
      expires_ms: Math.max(0, inv.expires - this.now()),
    });
  }

  private memberByName(p: Party, name: string): number {
    const k = key(name);
    for (const m of p.members.values()) if (key(m.name) === k) return m.heroId;
    throw new GameError('no_such_member', `${name.trim()} is not in your party`);
  }

  private mustBeOnline(heroId: number): Online {
    const o = this.online.get(heroId);
    if (!o) throw new GameError('offline', 'Not in the world');
    return o;
  }

  private nameOf(heroId: number): string {
    return this.online.get(heroId)?.name ?? this.memberOf.get(heroId)?.members.get(heroId)?.name ?? `#${heroId}`;
  }

  private onlineCount(p: Party): number {
    let n = 0;
    for (const m of p.members.values()) if (m.offlineSince === null) n++;
    return n;
  }

  /** The party as each member sees it. */
  private push(p: Party): void {
    const members = [...p.members.values()]
      .sort((a, b) => a.colour - b.colour)
      .map((m) => {
        const w = m.offlineSince === null ? this.host.whereIs(m.heroId) : undefined;
        return {
          id: m.heroId,
          name: m.name,
          colour: PARTY_COLOURS[m.colour],
          slot: m.colour,
          leader: m.heroId === p.leader,
          online: m.offlineSince === null,
          region: w?.region ?? null,
          channel: w?.channel ?? null,
        };
      });
    const msg = { t: 'party', id: p.id, leader: p.leader, max: this.opts.maxSize, members };
    for (const id of p.members.keys()) this.online.get(id)?.send(msg);
  }

  private note(p: Party, text: string, except?: number): void {
    for (const id of p.members.keys()) if (id !== except) this.online.get(id)?.send({ t: 'party_note', text });
  }

  private tell(heroId: number, text: string): void {
    this.online.get(heroId)?.send({ t: 'party_note', text });
  }
}
