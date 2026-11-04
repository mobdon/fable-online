import { GameBin, NamesBin, readI32 } from './fable-bin.js';

/**
 * Boasts. Before some quests the hero can boast at the podium (no damage, no weapons, naked,
 * and a few quest-specific ones). A kept boast adds gold to the quest card's reward. The
 * amounts are script constants in data/CompiledDefs/script.bin (the one CScriptDef,
 * SCRIPT_DEF), named <Prefix><Boast>BoastReward and ...BoastCost. Fable.exe still carries
 * the names, which is how these were matched. The prefixes are the quest scripts' own short
 * names; the table below ties each to its quest card.
 *
 * Only the names are kept here; the numbers are read from the player's own install.
 * The server treats card gold + every boast's reward as the most a quest can pay, and card
 * renown + every boast's cost as the most renown (what Cost does in game is not confirmed,
 * so it is counted as renown headroom rather than ignored).
 */
const CARD = 'OBJECT_QUEST_CARD_';
const STANDARD = ['Naked', 'NoDamage', 'NoWeapons'];

/** Script prefix -> quest card, and the boasts that prefix has (beyond the standard three). */
export const BOAST_QUESTS: { prefix: string; card: string; extra?: string[]; plain?: string[] }[] = [
  { prefix: 'AmbushTraders', card: 'AMBUSH_TRADERS', extra: ['KillAllGuards', 'KillAllTraders', 'KillNone', 'Unassisted'] },
  { prefix: 'Arena', card: 'ARENA', extra: ['ShowMercy', 'ShowNoMercy'] },
  { prefix: 'BanditCamp', card: 'BANDIT_CAMP', extra: ['KillNoBandits', 'KillManyBandits'] },
  { prefix: 'BountyHunt', card: 'BOUNTY_HUNT', extra: ['Decapitation', 'TimeLimit'] },
  { prefix: 'BreakSiege', card: 'BREAK_SIEGE' },
  { prefix: 'Graveyard', card: 'GRAVEYARD_SECRET_PASSAGE' },
  { prefix: 'HobbeCave', card: 'HOBBE_CAVE' },
  { prefix: 'HTContest', card: 'HOBBE_CONTEST' },
  { prefix: 'HTEvil', card: 'HANGING_TREE_EVIL' },
  { prefix: 'HTGood', card: 'HANGING_TREE_GOOD' },
  { prefix: 'MCC', card: 'MINION_CLIFFTOP_CHASE' },
  { prefix: 'MinionCamp', card: 'MINION_CAMP' },
  { prefix: 'OFEvil', card: 'PROTECT_FARM_EVIL' },
  { prefix: 'OFGood', card: 'PROTECT_FARM' },
  { prefix: 'Prison', card: 'PRISON_ESCAPE' },
  { prefix: 'RansomVictim', card: 'RANSOM_VICTIM', plain: ['VictimNoDamage', 'KillKidnapper', 'SaveVictim', 'HaveVictimKilled'] },
  { prefix: 'TraderConflictEvil', card: 'TRADER_CONFLICT_EVIL' },
  { prefix: 'TraderConflictGood', card: 'TRADER_CONFLICT_GOOD' },
  { prefix: 'TCE', card: 'TRADER_CONFLICT_EVIL', plain: ['KeepBanditFollowerAlive', 'MadeTimeLimit'] },
  { prefix: 'TCG', card: 'TRADER_CONFLICT_GOOD', plain: ['KillNoBandits', 'MadeTimeLimit'] },
  { prefix: 'TraderEscort', card: 'DARKWOOD_TRADER_ESCORT' },
  { prefix: 'WhiteBalv', card: 'WHITE_BALVERINE_KNOTHOLE_GLADE' },
  { prefix: 'SummoningShip', card: 'SUMMONING_THE_SHIP' },
];

export interface BoastBonus {
  /** Sum of every boast's gold reward for the quest. */
  gold: number;
  /** Sum of every boast's cost. */
  renown: number;
  /** How many boast constants were found (0 = none of this quest's names are in the file). */
  found: number;
}

/** The constant names a quest's boasts use: [reward, cost] pairs. */
export function boastNames(q: (typeof BOAST_QUESTS)[number]): [string, string][] {
  const out: [string, string][] = [];
  for (const b of [...STANDARD, ...(q.extra ?? [])]) {
    out.push([`${q.prefix}${b}BoastReward`, `${q.prefix}${b}BoastCost`]);
    // Some quests name them the other way round (AmbushTradersBoastRewardKillNone).
    out.push([`${q.prefix}BoastReward${b}`, `${q.prefix}BoastCost${b}`]);
  }
  for (const b of q.plain ?? []) out.push([`${q.prefix}${b}Reward`, `${q.prefix}${b}Cost`]);
  return out;
}

/** Card def name -> the most its boasts can add. Cards with no boasts are left out. */
export function extractBoasts(scriptBin: Buffer, namesBin: Buffer): Record<string, BoastBonus> {
  const g = new GameBin(scriptBin, new NamesBin(namesBin));
  const script = g.group('CScriptDef')[0];
  if (!script) return {};
  const own = g.own(script);
  const out: Record<string, BoastBonus> = {};
  for (const q of BOAST_QUESTS) {
    const card = CARD + q.card;
    const b = (out[card] ??= { gold: 0, renown: 0, found: 0 });
    for (const [reward, cost] of boastNames(q)) {
      const r = readI32(own, reward);
      const c = readI32(own, cost);
      // Negative or absurd values would only widen the cap; they are skipped.
      if (r !== undefined && r >= 0 && r <= 100_000) {
        b.gold += r;
        b.found++;
      }
      if (c !== undefined && c >= 0 && c <= 100_000) b.renown += c;
    }
  }
  for (const k of Object.keys(out)) if (!out[k]!.found) delete out[k];
  return out;
}
