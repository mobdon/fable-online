// Mutes and bans, keyed by account id. Saved to a JSON file so they survive a restart.
// When the persistence service lands (server skeleton), swap the file for its table.
import { readFileSync, writeFileSync, renameSync } from "node:fs";

interface Entry { until: number; reason: string; by: string }
interface Saved { mutes: Record<string, Entry>; bans: Record<string, Entry> }

const FOREVER = Number.MAX_SAFE_INTEGER;

export class Moderation {
  private mutes = new Map<string, Entry>();
  private bans = new Map<string, Entry>();
  private file: string | null;

  constructor(file: string | null) {
    this.file = file;
    if (!file) return;
    try {
      const s = JSON.parse(readFileSync(file, "utf8")) as Saved;
      for (const [k, v] of Object.entries(s.mutes ?? {})) this.mutes.set(k, v);
      for (const [k, v] of Object.entries(s.bans ?? {})) this.bans.set(k, v);
    } catch { /* no file yet */ }
  }

  mute(account: string, ms: number | null, reason: string, by: string, now = Date.now()) {
    this.mutes.set(account, { until: ms === null ? FOREVER : now + ms, reason, by });
    this.save();
  }
  unmute(account: string) { this.mutes.delete(account); this.save(); }
  ban(account: string, ms: number | null, reason: string, by: string, now = Date.now()) {
    this.bans.set(account, { until: ms === null ? FOREVER : now + ms, reason, by });
    this.save();
  }

  /** Milliseconds of mute left, or 0. */
  mutedFor(account: string, now = Date.now()): number {
    const e = this.mutes.get(account);
    if (!e) return 0;
    if (e.until <= now) { this.mutes.delete(account); this.save(); return 0; }
    return e.until - now;
  }
  banned(account: string, now = Date.now()): Entry | null {
    const e = this.bans.get(account);
    if (!e) return null;
    if (e.until <= now) { this.bans.delete(account); this.save(); return null; }
    return e;
  }

  private save() {
    if (!this.file) return;
    const out: Saved = { mutes: Object.fromEntries(this.mutes), bans: Object.fromEntries(this.bans) };
    const tmp = this.file + ".tmp";
    writeFileSync(tmp, JSON.stringify(out, null, 2));
    renameSync(tmp, this.file);
  }
}
