// Chat and name tag wire protocol, version 1.
// One JSON object per line ("NDJSON") over TCP. Every object has a "t" field.
// Spec with examples: shared/protocol/chat.md. Keep the two in sync.

export const PROTOCOL_VERSION = 1;
export const MAX_LINE_BYTES = 4096;   // a client line longer than this drops the connection
export const MAX_TEXT_CHARS = 200;    // one chat message

/** Channels a player can type into. "whisper" and "system" are server-made. */
export const TALK_CHANNELS = ["say", "region", "realm", "trade", "party", "guild"] as const;
export type TalkChannel = (typeof TALK_CHANNELS)[number];
export type Channel = TalkChannel | "whisper" | "system";

// ---- client -> server ---------------------------------------------------
export type ClientMsg =
  | { t: "hello"; v: number; token?: string; name?: string; dev?: { renown?: number; title?: string; guild?: string; pvp?: boolean } }
  | { t: "say"; ch: TalkChannel; text: string }
  | { t: "whisper"; to: string; text: string }
  | { t: "pos"; region: number; x: number; y: number; z: number }
  | { t: "ignore"; name: string; on: boolean }
  | { t: "report"; name: string; reason: string }
  | { t: "mod"; action: "mute" | "unmute" | "kick" | "ban"; name: string; minutes?: number; reason?: string }
  | { t: "ping"; n: number };

// ---- server -> client ---------------------------------------------------
export interface TagInfo {
  id: number;
  name: string;
  renown: number;
  title: string;     // Fable title, e.g. "Hero", "Arrow Master". Empty when none.
  guild: string;     // short guild tag, empty when none
  pvp: boolean;      // open-world PvP flag (unlocked by the Oakvale Guild mission)
  staff: StaffBadge; // from the account role the server signed into the login token; players can't set it
  x: number; y: number; z: number;
}
/** Shown above a staff member's name: "DEV" for admins, "GM" for game masters, "" for players. */
export type StaffBadge = "" | "DEV" | "GM";
export const staffBadge = (role: string): StaffBadge => (role === "admin" ? "DEV" : role === "mod" ? "GM" : "");
export type TagMove = Pick<TagInfo, "id" | "x" | "y" | "z">;

export type ServerMsg =
  | { t: "welcome"; v: number; id: number; name: string; renown: number; staff: StaffBadge; motd: string; channels: TalkChannel[] }
  | { t: "msg"; ch: Channel; from: string; fromId: number; text: string; ts: number; to?: string }
  | { t: "sys"; text: string; level: "info" | "warn" }
  | { t: "err"; code: ErrCode; text: string }
  | { t: "tags"; region: number; add: TagInfo[]; upd: TagInfo[]; move: TagMove[]; del: number[] }
  | { t: "pong"; n: number };

export type ErrCode =
  | "bad_message" | "not_logged_in" | "auth_failed" | "name_taken" | "bad_name"
  | "rate_limited" | "muted" | "too_long" | "duplicate" | "no_such_player"
  | "not_in_party" | "not_in_guild" | "forbidden" | "version";

const isStr = (v: unknown, max = 512): v is string => typeof v === "string" && v.length <= max;
const isNum = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

/** Parses one client line. Returns null for anything malformed, so callers never see junk. */
export function parseClientMsg(line: string): ClientMsg | null {
  let o: any;
  try { o = JSON.parse(line); } catch { return null; }
  if (!o || typeof o !== "object" || Array.isArray(o)) return null;
  switch (o.t) {
    case "hello":
      if (!isNum(o.v)) return null;
      if (o.token !== undefined && !isStr(o.token, 2048)) return null;
      if (o.name !== undefined && !isStr(o.name, 64)) return null;
      // "dev" lets test bots show renown, title and guild on a dev server. Ignored with token login.
      let dev: { renown?: number; title?: string; guild?: string; pvp?: boolean } | undefined;
      if (o.dev && typeof o.dev === "object") {
        dev = {
          renown: isNum(o.dev.renown) ? o.dev.renown : undefined,
          title: isStr(o.dev.title, 32) ? o.dev.title : undefined,
          guild: isStr(o.dev.guild, 5) ? o.dev.guild : undefined,
          pvp: typeof o.dev.pvp === "boolean" ? o.dev.pvp : undefined,
        };
      }
      return { t: "hello", v: o.v, token: o.token, name: o.name, dev };
    case "say":
      if (!TALK_CHANNELS.includes(o.ch) || !isStr(o.text, 1024)) return null;
      return { t: "say", ch: o.ch, text: o.text };
    case "whisper":
      if (!isStr(o.to, 64) || !isStr(o.text, 1024)) return null;
      return { t: "whisper", to: o.to, text: o.text };
    case "pos":
      if (!isNum(o.region) || !isNum(o.x) || !isNum(o.y) || !isNum(o.z)) return null;
      return { t: "pos", region: o.region >>> 0, x: o.x, y: o.y, z: o.z };
    case "ignore":
      if (!isStr(o.name, 64) || typeof o.on !== "boolean") return null;
      return { t: "ignore", name: o.name, on: o.on };
    case "report":
      if (!isStr(o.name, 64) || !isStr(o.reason, 300)) return null;
      return { t: "report", name: o.name, reason: o.reason };
    case "mod":
      if (!["mute", "unmute", "kick", "ban"].includes(o.action) || !isStr(o.name, 64)) return null;
      if (o.minutes !== undefined && !isNum(o.minutes)) return null;
      if (o.reason !== undefined && !isStr(o.reason, 300)) return null;
      return { t: "mod", action: o.action, name: o.name, minutes: o.minutes, reason: o.reason };
    case "ping":
      if (!isNum(o.n)) return null;
      return { t: "ping", n: o.n };
    default:
      return null;
  }
}

export const encode = (m: ServerMsg): string => JSON.stringify(m) + "\n";
