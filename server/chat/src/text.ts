// Cleaning player text and names before anyone else sees them.
import { MAX_TEXT_CHARS } from "./protocol.ts";

/** Hero names: 3 to 16 letters, digits, spaces, apostrophes or hyphens; starts with a letter. */
const NAME_RE = /^[A-Za-z][A-Za-z0-9' -]{1,14}[A-Za-z0-9]$/;
export const isValidName = (name: string): boolean => NAME_RE.test(name) && !/ {2}/.test(name);
export const nameKey = (name: string): string => name.trim().toLowerCase();

export type CleanResult = { ok: true; text: string } | { ok: false; reason: "empty" | "too_long" };

/**
 * Strips control characters, bidi overrides and zero-width tricks, collapses whitespace,
 * and limits length. The client font only draws Latin-1, so other characters become '?'
 * there; the server keeps them so a future font can show them.
 */
export function cleanText(raw: string): CleanResult {
  let s = raw
    .normalize("NFKC")
    .replace(/[\u0000-\u001F\u007F-\u009F]/g, " ")
    .replace(/[​-‏‪-‮⁠-⁩﻿]/g, "")
    .replace(/\s+/g, " ")
    .trim();
  if (s.length === 0) return { ok: false, reason: "empty" };
  if ([...s].length > MAX_TEXT_CHARS) return { ok: false, reason: "too_long" };
  return { ok: true, text: s };
}

/** Word filter. Matches whole words, ignoring case and common letter swaps (a→@, i→1, ...). */
export class WordFilter {
  private re: RegExp | null;
  constructor(words: string[]) {
    const parts = words.filter(Boolean).map((w) =>
      [...w.toLowerCase()].map((c) => {
        const alt: Record<string, string> = { a: "a@4", e: "e3", i: "i1!l", o: "o0", s: "s5$", t: "t7" };
        return alt[c] ? `[${alt[c].replace(/[$]/g, "\\$")}]` : c.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      }).join("+") + "+",
    );
    this.re = parts.length ? new RegExp(`(?<![a-z0-9])(?:${parts.join("|")})(?![a-z0-9])`, "gi") : null;
  }
  apply(text: string): string {
    return this.re ? text.replace(this.re, (m) => "*".repeat(m.length)) : text;
  }
}
