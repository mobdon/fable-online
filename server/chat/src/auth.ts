// Login tokens. The gateway (server skeleton) signs a token at login; chat only verifies it,
// so chat never sees passwords. Format: base64url(JSON claims) + "." + base64url(HMAC-SHA256).
import { createHmac, timingSafeEqual } from "node:crypto";

export type Role = "player" | "mod" | "admin";

export interface Claims {
  sub: string;       // account id
  name: string;      // hero name
  role: Role;
  renown?: number;   // starting renown; the region server updates it live
  title?: string;
  guild?: string;
  exp: number;       // unix seconds
}

const b64 = (b: Buffer) => b.toString("base64url");

export function signToken(claims: Claims, secret: string): string {
  const body = b64(Buffer.from(JSON.stringify(claims)));
  const sig = b64(createHmac("sha256", secret).update(body).digest());
  return `${body}.${sig}`;
}

export function verifyToken(token: string, secret: string, nowSec = Date.now() / 1000): Claims | null {
  const dot = token.indexOf(".");
  if (dot <= 0) return null;
  const body = token.slice(0, dot);
  const sig = Buffer.from(token.slice(dot + 1), "base64url");
  const want = createHmac("sha256", secret).update(body).digest();
  if (sig.length !== want.length || !timingSafeEqual(sig, want)) return null;
  let c: any;
  try { c = JSON.parse(Buffer.from(body, "base64url").toString("utf8")); } catch { return null; }
  if (!c || typeof c.sub !== "string" || typeof c.name !== "string" || typeof c.exp !== "number") return null;
  if (c.exp < nowSec) return null;
  if (!["player", "mod", "admin"].includes(c.role)) return null;
  return c as Claims;
}
