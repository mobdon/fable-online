import fs from 'node:fs';
import type { Database } from '../db/database.js';
import type { EventSink } from '../analytics/events.js';
import { MINT_REASONS, type Economy } from './economy.js';

/**
 * Quest rewards. Quests run on each player's own Fable, so the server can't see them
 * happen; it can only refuse to pay what a real quest could never pay. A claim names the
 * quest card (OBJECT_QUEST_CARD_*, whose gold, renown and reward items come from game.bin)
 * or a quest script name (shared/defs/quests.json, paid at most the default cap). A claim
 * is paid only when all of these hold, otherwise nothing is paid and the attempt is logged:
 *  - the quest is a known card or script, or has an explicit rule,
 *  - the hero hasn't been paid for it already (or the repeat interval has passed),
 *  - the gold is within the quest's cap (and the ledger's quest_reward cap),
 *  - every item is one the rule allows, and is not staff-only,
 *  - the hero is under the daily quest-gold cap.
 * The cap is a ceiling: a claim over it is rejected whole, not trimmed, because it means
 * the client is lying about the reward.
 */
export interface QuestRule {
  maxGold: number;
  maxRenown?: number;
  /** Items this quest may hand out. Missing = none. */
  items?: string[];
  /** Repeatable quests (minigames, bounties) may be paid again after this long. */
  repeatMs?: number;
  /** Off = never pay (e.g. a quest found to be exploitable). */
  enabled?: boolean;
}

export interface QuestClaim {
  quest: string;
  gold: number;
  renown?: number;
  items?: string[];
}

export type ClaimResult = { ok: true; gold: number; renown: number; items: string[] } | { ok: false; reason: string };

const DAY = 24 * 3600_000;
const QUEST_RE = /^[A-Za-z0-9_]{1,64}$/;

export class QuestRewards {
  private known: Set<string>;

  constructor(
    private db: Database,
    private economy: Economy,
    private events: EventSink,
    private rules: Record<string, QuestRule> = {},
    knownQuests: Iterable<string> = [],
    private cards: Record<string, { gold: number; renown: number; items: string[] }> = {},
    private opts = { defaultMaxGold: 2_000, dailyGoldCap: 50_000 },
  ) {
    this.known = new Set(knownQuests);
  }

  static loadKnown(file: string | undefined): string[] {
    if (!file || !fs.existsSync(file)) return [];
    const d = JSON.parse(fs.readFileSync(file, 'utf8')) as { quests?: string[] };
    return d.quests ?? [];
  }

  static loadRules(file: string | undefined): Record<string, QuestRule> {
    if (!file || !fs.existsSync(file)) return {};
    return (JSON.parse(fs.readFileSync(file, 'utf8')) as { quests?: Record<string, QuestRule> }).quests ?? {};
  }

  rule(quest: string): QuestRule | undefined {
    const r = this.rules[quest];
    if (r) return r;
    const card = this.cards[quest];
    if (card) return { maxGold: card.gold, maxRenown: card.renown, items: card.items };
    return this.known.has(quest) ? { maxGold: this.opts.defaultMaxGold, maxRenown: 0 } : undefined;
  }

  claim(heroId: number, c: QuestClaim, now = Date.now()): ClaimResult {
    const reject = (reason: string): ClaimResult => {
      this.events.emit({ type: 'quest_reward_rejected', heroId, quest: String(c.quest).slice(0, 64), gold: Number(c.gold) || 0, reason });
      return { ok: false, reason };
    };
    if (typeof c.quest !== 'string' || !QUEST_RE.test(c.quest)) return reject('bad_quest');
    const gold = c.gold;
    if (!Number.isSafeInteger(gold) || gold < 0) return reject('bad_gold');
    const items = c.items ?? [];
    if (!Array.isArray(items) || items.length > 8 || items.some((i) => typeof i !== 'string')) return reject('bad_items');

    const rule = this.rule(c.quest);
    if (!rule) return reject('unknown_quest');
    if (rule.enabled === false) return reject('quest_disabled');
    if (gold > Math.min(rule.maxGold, MINT_REASONS.quest_reward)) return reject('over_cap');
    const renown = c.renown ?? 0;
    if (!Number.isSafeInteger(renown) || renown < 0) return reject('bad_renown');
    if (renown > (rule.maxRenown ?? 0)) return reject('over_cap');
    // Each item at most as many times as the rule lists it: a quest that gives one sword
    // can't be claimed as eight.
    const left = new Map<string, number>();
    for (const it of rule.items ?? []) left.set(it, (left.get(it) ?? 0) + 1);
    for (const it of items) {
      const n = left.get(it) ?? 0;
      if (n === 0 || !this.economy.catalog.has(it)) return reject('item_not_allowed');
      left.set(it, n - 1);
      if (this.economy.catalog.get(it).staffOnly) {
        this.events.emit({ type: 'staff_item_blocked', heroId, defId: it, action: `quest:${c.quest}`, actor: `hero:${heroId}` });
        return reject('item_not_allowed');
      }
    }

    return this.db.tx(() => {
      const last = this.db.get<{ at: number }>('SELECT MAX(claimed_at) AS at FROM quest_claims WHERE hero_id = ? AND quest = ?', heroId, c.quest)?.at;
      if (last != null && (rule.repeatMs === undefined || now - last < rule.repeatMs)) return reject('already_claimed');
      const today = this.db.get<{ s: number }>('SELECT COALESCE(SUM(gold),0) AS s FROM quest_claims WHERE hero_id = ? AND claimed_at > ?', heroId, now - DAY)!.s;
      if (today + gold > this.opts.dailyGoldCap) return reject('daily_cap');

      const { lastInsertRowid: claimId } = this.db.run(
        'INSERT INTO quest_claims (hero_id, quest, gold, renown, claimed_at) VALUES (?, ?, ?, ?, ?)',
        heroId,
        c.quest,
        gold,
        renown,
        now,
      );
      const key = `quest:${heroId}:${claimId}`;
      // Any failure below throws and rolls the claim row back with the payment.
      if (gold > 0) {
        const r = this.economy.grant(heroId, gold, 'quest_reward', `${key}:gold`);
        this.db.run('UPDATE quest_claims SET tx_id = ? WHERE id = ?', r.txId, claimId);
      }
      items.forEach((it, i) => this.economy.mintItem(heroId, it, 1, 'quest_reward', `${key}:item:${i}`));
      if (renown > 0) this.db.run('UPDATE heroes SET renown = renown + ? WHERE id = ?', renown, heroId);
      this.events.emit({ type: 'quest_reward', heroId, quest: c.quest, gold, items: items.length });
      return { ok: true as const, gold, renown, items };
    });
  }
}
