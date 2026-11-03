// The chat and name tag logic, with no sockets in it, so tests can drive it directly.
// server.ts puts it on TCP. The region servers (phase 5) will call updateHero() with
// authoritative positions, renown and PvP flags.
import {
  PROTOCOL_VERSION, TALK_CHANNELS,
  type ClientMsg, type ServerMsg, type TalkChannel, type ErrCode, type TagInfo, type TagMove,
} from "./protocol.ts";
import { verifyToken, type Role } from "./auth.ts";
import { cleanText, isValidName, nameKey, WordFilter } from "./text.ts";
import { PlayerRate, DEFAULT_RATE, type RateConfig } from "./ratelimit.ts";
import { Moderation } from "./moderation.ts";

export interface HubConfig {
  /** HMAC secret shared with the gateway. null = dev mode: anyone can log in by name. */
  tokenSecret: string | null;
  /** Dev only: accept "pos" from clients. Off in production, where region servers own positions. */
  trustClientPositions: boolean;
  sayRadius: number;      // game units (about a metre each)
  tagRadius: number;      // how far away a name tag is sent
  moveEpsilon: number;    // smaller moves than this are not sent
  motd: string;
  bannedWords: string[];
  rate: RateConfig;
  moderationFile: string | null;
}

export const DEFAULT_CONFIG: HubConfig = {
  tokenSecret: null,
  trustClientPositions: true,
  sayRadius: 30,
  tagRadius: 60,
  moveEpsilon: 0.05,
  motd: "Welcome to Albion. Press Enter to chat, /help for commands.",
  bannedWords: [],
  rate: DEFAULT_RATE,
  moderationFile: null,
};

/** Analytics hook. The analytics thread plugs its event logger in here. */
export type EventSink = (event: string, data: Record<string, unknown>) => void;

export interface Conn {
  send(m: ServerMsg): void;
  close(reason: string): void;
}

export interface HeroState {
  region: number; x: number; y: number; z: number;
  renown: number; title: string; guild: string; pvp: boolean;
  /** Guild chat goes to heroes with the same guildId. Each guild, sub guilds included, has its own. */
  guildId: string;
}

interface Session extends HeroState {
  id: number;
  conn: Conn;
  account: string;
  name: string;
  key: string;
  role: Role;
  hasPos: boolean;
  party: string | null;
  ignore: Set<string>;
  replyTo: string | null;
  rate: PlayerRate;
  seen: Map<number, TagInfo>;   // name tags this client currently has, as last sent
  tagsDirty: boolean;           // renown/title/guild/pvp changed: resend full tag
}

export class ChatHub {
  readonly cfg: HubConfig;
  private emit: EventSink;
  private now: () => number;
  private nextId = 1;
  private pending = new Set<Conn>();
  private byConn = new Map<Conn, Session>();
  private byKey = new Map<string, Session>();
  private filter: WordFilter;
  readonly moderation: Moderation;

  constructor(cfg: Partial<HubConfig> = {}, emit: EventSink = () => {}, now: () => number = Date.now) {
    // Undefined settings fall back to the defaults instead of overriding them.
    const given = Object.fromEntries(Object.entries(cfg).filter(([, v]) => v !== undefined));
    this.cfg = { ...DEFAULT_CONFIG, ...given };
    this.emit = emit;
    this.now = now;
    this.filter = new WordFilter(this.cfg.bannedWords);
    this.moderation = new Moderation(this.cfg.moderationFile);
  }

  get online(): number { return this.byKey.size; }

  open(conn: Conn) { this.pending.add(conn); }

  closed(conn: Conn) {
    this.pending.delete(conn);
    const s = this.byConn.get(conn);
    if (!s) return;
    this.byConn.delete(conn);
    this.byKey.delete(s.key);
    this.emit("chat.logout", { account: s.account, name: s.name });
    for (const o of this.byKey.values()) {
      if (o.seen.delete(s.id)) o.conn.send({ t: "tags", region: o.region, add: [], upd: [], move: [], del: [s.id] });
    }
  }

  handle(conn: Conn, m: ClientMsg) {
    const s = this.byConn.get(conn);
    if (m.t === "ping") return conn.send({ t: "pong", n: m.n });
    if (m.t === "hello") return s ? undefined : this.login(conn, m);
    if (!s) return this.err(conn, "not_logged_in", "Say hello first.");
    switch (m.t) {
      case "say": return this.say(s, m.ch, m.text);
      case "whisper": return this.whisper(s, m.to, m.text);
      case "pos":
        if (!this.cfg.trustClientPositions) return;  // region server owns positions
        return this.updateHero(s.name, { region: m.region, x: m.x, y: m.y, z: m.z });
      case "ignore": return this.setIgnore(s, m.name, m.on);
      case "report": return this.report(s, m.name, m.reason);
      case "mod": return this.mod(s, m);
    }
  }

  // ---- login --------------------------------------------------------------
  private login(conn: Conn, m: Extract<ClientMsg, { t: "hello" }>) {
    if (m.v !== PROTOCOL_VERSION) {
      this.err(conn, "version", `Server speaks chat protocol ${PROTOCOL_VERSION}; update the mod.`);
      return conn.close("version");
    }
    let account: string, name: string, role: Role = "player";
    let renown = 0, title = "", guild = "", guildId = "";
    if (this.cfg.tokenSecret) {
      const c = m.token ? verifyToken(m.token, this.cfg.tokenSecret) : null;
      if (!c) { this.err(conn, "auth_failed", "Login expired. Restart from the launcher."); return conn.close("auth"); }
      ({ sub: account, name, role } = c);
      renown = c.renown ?? 0; title = c.title ?? ""; guild = c.guild ?? ""; guildId = c.guildId ?? guild;
    } else {
      name = (m.name ?? "").trim();
      account = "dev:" + nameKey(name);
      if (/^(mod|admin)[ -]/i.test(name)) role = "mod";  // dev mode only: test moderation by name
      renown = m.dev?.renown ?? 0; title = m.dev?.title ?? ""; guild = m.dev?.guild ?? ""; guildId = guild;
    }
    if (!isValidName(name)) { this.err(conn, "bad_name", "Names are 3-16 letters, digits, spaces, ' or -."); return conn.close("name"); }
    const ban = this.moderation.banned(account, this.now());
    if (ban) { this.err(conn, "forbidden", `Banned: ${ban.reason}`); return conn.close("banned"); }
    const key = nameKey(name);
    const old = this.byKey.get(key);
    if (old) {
      if (old.account !== account) { this.err(conn, "name_taken", "That name is online."); return conn.close("name_taken"); }
      old.conn.close("logged in elsewhere");  // same account reconnecting: newest wins
      this.closed(old.conn);
    }
    this.pending.delete(conn);
    const s: Session = {
      id: this.nextId++, conn, account, name, key, role,
      region: 0, x: 0, y: 0, z: 0, hasPos: false, renown, title, guild, guildId, pvp: false,
      party: null, ignore: new Set(), replyTo: null,
      rate: new PlayerRate(this.cfg.rate, this.now()), seen: new Map(), tagsDirty: false,
    };
    if (!this.cfg.tokenSecret && m.dev?.pvp) s.pvp = true;
    this.byConn.set(conn, s);
    this.byKey.set(key, s);
    conn.send({ t: "welcome", v: PROTOCOL_VERSION, id: s.id, name, renown, motd: this.cfg.motd, channels: [...TALK_CHANNELS] });
    this.emit("chat.login", { account, name, role });
  }

  // ---- talking ------------------------------------------------------------
  private gate(s: Session, ch: TalkChannel | "whisper", raw: string): string | null {
    const muted = this.moderation.mutedFor(s.account, this.now());
    if (muted > 0) { this.err(s.conn, "muted", `You are muted for ${fmtMs(muted)}.`); return null; }
    const c = cleanText(raw);
    if (!c.ok) { if (c.reason === "too_long") this.err(s.conn, "too_long", "Message too long (200 max)."); return null; }
    const v = s.rate.check(ch, c.text, this.now());
    if (!v.ok) {
      if (v.autoMute) {
        this.moderation.mute(s.account, this.cfg.rate.autoMuteMs, "spam (automatic)", "system", this.now());
        this.emit("chat.automute", { account: s.account, name: s.name });
        this.err(s.conn, "muted", `Muted for ${fmtMs(this.cfg.rate.autoMuteMs)} for spamming.`);
      } else if (v.why === "duplicate") {
        this.err(s.conn, "duplicate", "You just said that.");
      } else {
        this.err(s.conn, "rate_limited", `Slow down: wait ${Math.ceil(v.retryMs / 1000)}s.`);
      }
      return null;
    }
    return this.filter.apply(c.text);
  }

  private say(s: Session, ch: TalkChannel, raw: string) {
    let targets: Session[];
    switch (ch) {
      case "say": {
        const r2 = this.cfg.sayRadius ** 2;
        targets = this.all().filter((o) => o.hasPos && s.hasPos && o.region === s.region && dist2(o, s) <= r2);
        if (!s.hasPos) targets = [s];
        break;
      }
      case "region": targets = this.all().filter((o) => o.region === s.region); break;
      case "realm": case "trade": targets = this.all(); break;
      case "party":
        if (!s.party) return this.err(s.conn, "not_in_party", "You are not in a party.");
        targets = this.all().filter((o) => o.party === s.party); break;
      case "guild":
        if (!s.guildId) return this.err(s.conn, "not_in_guild", "You are not in a guild.");
        targets = this.all().filter((o) => o.guildId === s.guildId); break;
    }
    const text = this.gate(s, ch, raw);
    if (text === null) return;
    const out: ServerMsg = { t: "msg", ch, from: s.name, fromId: s.id, text, ts: this.now() };
    for (const o of targets) if (o === s || !o.ignore.has(s.key)) o.conn.send(out);
    this.emit("chat.message", { ch, account: s.account, name: s.name, region: s.region, len: text.length, heard: targets.length });
  }

  private whisper(s: Session, toName: string, raw: string) {
    const to = this.byKey.get(nameKey(toName));
    if (!to || to === s) return this.err(s.conn, "no_such_player", `${toName} is not online.`);
    const text = this.gate(s, "whisper", raw);
    if (text === null) return;
    const ts = this.now();
    // An ignoring player just doesn't receive it; the sender isn't told, so ignore can't be probed.
    if (!to.ignore.has(s.key)) {
      to.conn.send({ t: "msg", ch: "whisper", from: s.name, fromId: s.id, text, ts, to: to.name });
      to.replyTo = s.key;
    }
    s.conn.send({ t: "msg", ch: "whisper", from: s.name, fromId: s.id, text, ts, to: to.name });
    this.emit("chat.message", { ch: "whisper", account: s.account, name: s.name, region: s.region, len: text.length, heard: 1 });
  }

  private setIgnore(s: Session, name: string, on: boolean) {
    const k = nameKey(name);
    if (on) s.ignore.add(k); else s.ignore.delete(k);
    this.sys(s, on ? `Ignoring ${name}.` : `No longer ignoring ${name}.`);
  }

  private report(s: Session, name: string, reason: string) {
    const t = this.byKey.get(nameKey(name));
    this.emit("chat.report", { by: s.account, target: t?.account ?? null, targetName: name, reason: reason.slice(0, 300) });
    this.sys(s, "Report sent. Thank you.");
  }

  private mod(s: Session, m: Extract<ClientMsg, { t: "mod" }>) {
    if (s.role === "player") return this.err(s.conn, "forbidden", "Moderators only.");
    const t = this.byKey.get(nameKey(m.name));
    if (!t) return this.err(s.conn, "no_such_player", `${m.name} is not online.`);
    if (t.role === "admin" && s.role !== "admin") return this.err(s.conn, "forbidden", "Can't moderate an admin.");
    const ms = m.minutes && m.minutes > 0 ? m.minutes * 60_000 : null;
    const reason = m.reason ?? "";
    switch (m.action) {
      case "mute":
        this.moderation.mute(t.account, ms ?? 15 * 60_000, reason, s.account, this.now());
        this.sys(t, `You were muted for ${fmtMs(ms ?? 15 * 60_000)}${reason ? ": " + reason : ""}.`, "warn");
        break;
      case "unmute": this.moderation.unmute(t.account); this.sys(t, "You can talk again."); break;
      case "kick": this.sys(t, "Kicked by a moderator.", "warn"); t.conn.close("kicked"); this.closed(t.conn); break;
      case "ban":
        this.moderation.ban(t.account, ms, reason || "no reason given", s.account, this.now());
        this.sys(t, "Banned by a moderator.", "warn"); t.conn.close("banned"); this.closed(t.conn); break;
    }
    this.sys(s, `${m.action} ${t.name}: done.`);
    this.emit("chat.mod", { by: s.account, action: m.action, target: t.account, minutes: m.minutes ?? null, reason });
  }

  // ---- hooks for the region server and the party system --------------------
  /** Region servers call this with authoritative state. Unknown fields keep their value. */
  updateHero(name: string, st: Partial<HeroState>): boolean {
    const s = this.byKey.get(nameKey(name));
    if (!s) return false;
    if (st.region !== undefined && st.region !== s.region) {
      s.region = st.region;
      s.seen.clear();  // client drops tags on region change; resend everything
    }
    if (st.x !== undefined) { s.x = st.x; s.y = st.y ?? s.y; s.z = st.z ?? s.z; s.hasPos = true; }
    if (st.guildId !== undefined) s.guildId = st.guildId;  // joining or leaving a guild: chat follows at once
    for (const k of ["renown", "title", "guild", "pvp"] as const) {
      if (st[k] !== undefined && st[k] !== s[k]) { (s as any)[k] = st[k]; s.tagsDirty = true; }
    }
    return true;
  }

  setParty(name: string, party: string | null): boolean {
    const s = this.byKey.get(nameKey(name));
    if (!s) return false;
    s.party = party;
    return true;
  }

  /** Called a few times a second: sends each player the name tags around them, as diffs. */
  tick() {
    const byRegion = new Map<number, Session[]>();
    for (const s of this.byKey.values()) {
      if (!s.hasPos) continue;
      let l = byRegion.get(s.region);
      if (!l) byRegion.set(s.region, (l = []));
      l.push(s);
    }
    const r2 = this.cfg.tagRadius ** 2, eps2 = this.cfg.moveEpsilon ** 2;
    for (const s of this.byKey.values()) {
      const add: TagInfo[] = [], upd: TagInfo[] = [], move: TagMove[] = [], del: number[] = [];
      const near = new Set<number>();
      if (s.hasPos) {
        for (const o of byRegion.get(s.region) ?? []) {
          if (o === s || dist2(o, s) > r2) continue;
          near.add(o.id);
          const was = s.seen.get(o.id);
          const now = tagOf(o);
          if (!was) add.push(now);
          else if (o.tagsDirty) upd.push(now);
          else if (dist2(was, now) > eps2) move.push({ id: o.id, x: now.x, y: now.y, z: now.z });
          else continue;
          s.seen.set(o.id, now);
        }
      }
      for (const id of s.seen.keys()) if (!near.has(id)) { del.push(id); s.seen.delete(id); }
      if (add.length || upd.length || move.length || del.length) s.conn.send({ t: "tags", region: s.region, add, upd, move, del });
    }
    for (const s of this.byKey.values()) s.tagsDirty = false;
  }

  // ---- helpers ------------------------------------------------------------
  private all(): Session[] { return [...this.byKey.values()]; }
  private err(conn: Conn, code: ErrCode, text: string) { conn.send({ t: "err", code, text }); }
  private sys(s: Session, text: string, level: "info" | "warn" = "info") { s.conn.send({ t: "sys", text, level }); }
}

const dist2 = (a: { x: number; y: number; z: number }, b: { x: number; y: number; z: number }) =>
  (a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2;

const round = (v: number) => Math.round(v * 100) / 100;
const tagOf = (s: Session): TagInfo => ({
  id: s.id, name: s.name, renown: s.renown, title: s.title, guild: s.guild, pvp: s.pvp,
  x: round(s.x), y: round(s.y), z: round(s.z),
});

function fmtMs(ms: number): string {
  if (ms > 365 * 86_400_000) return "good";
  const m = Math.ceil(ms / 60_000);
  return m >= 60 ? `${Math.round(m / 60)}h` : `${m}m`;
}
