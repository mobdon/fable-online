/**
 * Crime, fine, bribe and prison rules. Every number lives here so it can be tuned without
 * touching the code, and the choices that are still open are switches (see `open`).
 *
 * Crime kinds and their on-screen text come from Fable's own crime pipeline: the server
 * only sends text ids (TEXT_GUI_CRIME_*, TEXT_AI_GUARD_*) and each client resolves them
 * from its own text.big, so no game text is shipped.
 */

export type CrimeKind =
  | 'weapon_out'
  | 'trespass'
  | 'vandalism'
  | 'lockpicking'
  | 'pickpocket'
  | 'theft'
  | 'assault'
  | 'gbh'
  | 'murder'
  | 'murder_player';

/** Which guard bark family a witnessed crime of this kind triggers. */
export type Severity = 'minor' | 'moderate' | 'serious';

export interface CrimeDef {
  /** TEXT_GUI_CRIME_* line on the fine screen: "%1 x <crime> (%2 gold)". */
  textId: string;
  severity: Severity;
  /** Fine for the first offence. Item crimes add `valueFactor` x the item's value. */
  fine: number;
  valueFactor: number;
  /** Item crimes: never less than this. */
  minFine: number;
  /** Wanted level this crime puts the hero on by itself (1..4). */
  level: 1 | 2 | 3 | 4;
  /** Minutes thrown out of the town when it is settled at level 2 (0 = not thrown out). */
  kickMinutes: number;
  /** Forgiven after `pettyForgiveMinutes` of online play when unpaid. */
  petty: boolean;
  /** A bribe can clear it. */
  bribable: boolean;
  /** Can be settled with gold at all (murder of a player can't). */
  payable: boolean;
  /** Recorded without a witness (the server saw it happen in combat). */
  needsWitness: boolean;
}

export interface Jurisdiction {
  /** Town id, e.g. "bowerstone". */
  id: string;
  /** Level scripts that belong to the town. Guards only act inside their own town. */
  regions: string[];
  /** Where a hero thrown out of town is put. */
  kickTo: string;
}

export interface LawRules {
  crimes: Record<CrimeKind, CrimeDef>;
  /** A repeat of the same crime within `repeatWindowMs` costs this many times the last fine. */
  repeatFactor: number;
  repeatWindowMs: number;
  maxFine: number;
  /** Unpaid totals that raise the wanted level whatever the crimes were: [gold, level]. */
  levelByTotal: [number, 2 | 3 | 4][];
  pettyForgiveMinutes: number;

  bribe: {
    /** Bribe = max(min, factor x unpaid bribable fines), doubled per bribe that real day. */
    factor: number;
    min: number;
    /** Highest wanted level a bribe can clear. */
    maxLevel: number;
    /** After a bribe, that town's guards ignore petty crimes for this long. */
    blindMinutes: number;
  };

  jail: {
    region: string;
    /** Release point. */
    releaseTo: string;
    /** Sentence: this many minutes per 1,000 gold of unpaid fines, between min and max. */
    minutesPer1000: number;
    minMinutes: number;
    maxMinutes: number;
    /** Paying out early costs the fines plus this share. */
    earlyReleaseSurcharge: number;
    /** This many jail trips inside `repeatWindowDays` sends the next one to Bargate instead. */
    tripsBeforeBargate: number;
    repeatWindowDays: number;
  };

  bargate: {
    /** Cell blocks heroes are held in (shared). The first that exists in the region table is used. */
    cells: string[];
    /** Regions a prisoner may walk between. */
    grounds: string[];
    releaseTo: string;
    firstMinutes: number;
    /** Each repeat inside `repeatWindowDays` multiplies the sentence by this. */
    repeatFactor: number;
    maxMinutes: number;
    repeatWindowDays: number;
    /** Paying in the Warden's Office halves what's left, once per sentence, for this much gold per minute halved. */
    wardenGoldPerMinute: number;
    /** 3 or more Bargate trips in a week: part of the time is served in the Torture Chamber. */
    tortureAfterTrips: number;
    tortureRegion: string;
  };

  stealing: {
    /** Chance a steal is spotted when no guard has a clear view. */
    baseSpotChance: number;
    /** Added when a guard can see the hero. */
    guardInViewBonus: number;
    /** Added by day. */
    dayBonus: number;
    /** Added per steal in the last hour. */
    repeatBonus: number;
    /** Taken off per equipped stealth armour piece. */
    stealthPerPiece: number;
    /** The client's own "spotted stealing" event adds this (it can never lower the chance). */
    clientSpottedBonus: number;
    minChance: number;
    maxChance: number;
    /** Value a hero may steal per real day. */
    dailyValueCap: number;
    /** Seconds between steal attempts. */
    cooldownSec: number;
    /** Stolen mark length: marked items can't go to normal traders or other players. */
    markHours: number;
    /** The fence pays this share of value for marked items. */
    fenceRatio: number;
    /** Stolen value the fence takes per hero per real day. */
    fenceDailyCap: number;
    /** Copies of one item a hero can take from one shop per real day. */
    stockPerItem: number;
  };

  guards: {
    /** A guard walks up and stops this close to ask for the fine. */
    talkRange: number;
    /** A hero who doesn't answer in this long, or walks this far away, is running from the law. */
    answerMs: number;
    walkAwayRange: number;
    /** A pursuing guard this close for this long makes the arrest. */
    arrestRange: number;
    arrestHoldMs: number;
    /** Weapon-out warnings, this far apart. The third ignored warning is the crime. */
    warningGapMs: number;
    /** A running criminal out of every guard's sight this long is lost (the record stays). */
    loseMs: number;
    /** A villager who saw a crime tells the nearest guard after this long. */
    villagerReportMs: number;
    /** Guards in a town only see this far for crimes (heroes further off go unseen). */
    sight: number;
    fovDeg: number;
  };

  /** Shops, Cullis Gates and property are refused from this wanted level up. */
  servicesBlockedAt: number;

  /** Every town with guards, by region. Wilderness has no guards. */
  jurisdictions: Jurisdiction[];

  /**
   * Choices still open in the design. Each is a switch so the answer is a config change.
   *  - pickpocketPlayers: pickpocketing other heroes (recommended: never).
   *  - playerBounty: murdering a player also puts a claimable bounty on the killer.
   *  - bargateEscape: the Bargate escape route as a minigame in the first version.
   */
  open: {
    pickpocketPlayers: boolean;
    playerBounty: boolean;
    bargateEscape: boolean;
    /** Escaping takes this share off the time left; getting caught multiplies it. */
    escapeReward: number;
    escapeCaughtFactor: number;
  };
}

const crime = (textId: string, severity: Severity, fine: number, level: CrimeDef['level'], more: Partial<CrimeDef> = {}): CrimeDef => ({
  textId,
  severity,
  fine,
  valueFactor: 0,
  minFine: 0,
  level,
  kickMinutes: 0,
  petty: false,
  bribable: true,
  payable: true,
  needsWitness: true,
  ...more,
});

/** Starting numbers from the property and crime design draft. Tuned once real players are in. */
export const DEFAULT_RULES: LawRules = {
  crimes: {
    weapon_out: crime('TEXT_GUI_CRIME_WEAPON_OUT', 'minor', 20, 1, { petty: true }),
    trespass: crime('TEXT_GUI_CRIME_TRESPASS', 'minor', 50, 2, { petty: true, kickMinutes: 10 }),
    vandalism: crime('TEXT_GUI_CRIME_VANDALISM', 'moderate', 50, 1, { valueFactor: 1 }),
    lockpicking: crime('TEXT_GUI_CRIME_LOCKPICKING', 'moderate', 100, 1),
    pickpocket: crime('TEXT_GUI_CRIME_PICK_POCKETS', 'moderate', 150, 1, { valueFactor: 2 }),
    theft: crime('TEXT_GUI_CRIME_THEFT', 'moderate', 0, 1, { valueFactor: 2, minFine: 100 }),
    assault: crime('TEXT_GUI_CRIME_ASSAULT', 'serious', 300, 2, { kickMinutes: 10 }),
    gbh: crime('TEXT_GUI_CRIME_GBH', 'serious', 800, 2, { kickMinutes: 30 }),
    murder: crime('TEXT_GUI_CRIME_MURDER', 'serious', 2000, 3, { bribable: false }),
    murder_player: crime('TEXT_GUI_CRIME_MURDER', 'serious', 0, 4, { bribable: false, payable: false, needsWitness: false }),
  },
  repeatFactor: 1.5,
  repeatWindowMs: 24 * 3600_000,
  maxFine: 100_000,
  levelByTotal: [
    [1500, 2],
    [5000, 3],
  ],
  pettyForgiveMinutes: 60,

  bribe: { factor: 1.5, min: 200, maxLevel: 2, blindMinutes: 5 },

  jail: {
    region: 'BowerstoneJail',
    releaseTo: 'BowerstonePosh',
    minutesPer1000: 5,
    minMinutes: 2,
    maxMinutes: 15,
    earlyReleaseSurcharge: 0.25,
    tripsBeforeBargate: 3,
    repeatWindowDays: 7,
  },

  bargate: {
    cells: ['PrisonCells_1', 'PrisonCells_2', 'PrisonCells_3'],
    grounds: ['PrisonCells_1', 'PrisonCells_2', 'PrisonCells_3', 'PrisonCourtyard', 'PrisonOffice', 'PrisonTortureChamber'],
    releaseTo: 'PrisonPath',
    firstMinutes: 20,
    repeatFactor: 2,
    maxMinutes: 120,
    repeatWindowDays: 7,
    wardenGoldPerMinute: 50,
    tortureAfterTrips: 3,
    tortureRegion: 'PrisonTortureChamber',
  },

  stealing: {
    baseSpotChance: 0.25,
    guardInViewBonus: 0.6,
    dayBonus: 0.1,
    repeatBonus: 0.05,
    stealthPerPiece: 0.03,
    clientSpottedBonus: 0.5,
    minChance: 0.05,
    maxChance: 0.95,
    dailyValueCap: 2000,
    cooldownSec: 10,
    markHours: 72,
    fenceRatio: 0.25,
    fenceDailyCap: 2000,
    stockPerItem: 1,
  },

  guards: {
    talkRange: 2.2,
    answerMs: 20_000,
    walkAwayRange: 12,
    arrestRange: 2.6,
    arrestHoldMs: 1200,
    warningGapMs: 5000,
    loseMs: 30_000,
    villagerReportMs: 4000,
    sight: 18,
    fovDeg: 200,
  },

  servicesBlockedAt: 2,

  // Region lists from FinalAlbion.wld. kickTo values for towns whose exits aren't in the
  // region graph yet are the nearest outdoor area by Fable's map; check them in game.
  jurisdictions: [
    {
      id: 'bowerstone',
      regions: ['BowerstoneSlums', 'BowerstoneSlums_v2', 'BowerstonePosh', 'BowerstoneBridge', 'BowerstoneTavernCellar', 'MayorsHouseHallway', 'MayorsHouseBedroom'],
      kickTo: 'LookoutPoint',
    },
    { id: 'oakvale', regions: ['OakValeWest_v2', 'OakvaleMemorialGarden_v2'], kickTo: 'BarrowFields' },
    { id: 'knothole_glade', regions: ['KnotholeGlade', 'KnotholeGladeMainTown'], kickTo: 'Witchwood_4' },
    { id: 'hook_coast', regions: ['HookCoast'], kickTo: 'Darkwood' },
    { id: 'snowspire', regions: ['SnowspireVillage'], kickTo: 'NorthernWastes3' },
  ],

  open: {
    pickpocketPlayers: false,
    playerBounty: false,
    bargateEscape: false,
    escapeReward: 0.25,
    escapeCaughtFactor: 2,
  },
};

/** Deep-merge overrides (e.g. from content/law.json) onto the defaults. */
export function mergeRules(over: unknown, base: LawRules = DEFAULT_RULES): LawRules {
  const merge = (a: unknown, b: unknown): unknown => {
    if (b === undefined) return a;
    if (Array.isArray(a) || Array.isArray(b) || typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return b;
    const out: Record<string, unknown> = { ...(a as Record<string, unknown>) };
    for (const [k, v] of Object.entries(b as Record<string, unknown>)) out[k] = merge(out[k], v);
    return out;
  };
  return merge(base, over) as LawRules;
}

/**
 * Guard bark ids, picked by the server and voiced by the client from its own text.big and
 * sound banks: TEXT_AI_GUARD_<family>_<NN>_<voice>. Two voices exist (LEV1GRD1, LEV2GRD1);
 * each guard keeps one voice, picked from its id.
 */
export const BARKS = {
  minor: 'SEEN_MINOR_CRIME',
  moderate: 'SEEN_MODERATE_CRIME',
  serious: 'SEEN_SERIOUS_CRIME',
  criminal: 'SEEN_CRIMINAL',
  weapon: ['WARNING_WEAPON_OUT_FIRST', 'WARNING_WEAPON_OUT_SECOND', 'WARNING_WEAPON_OUT_THIRD'],
  trespass: ['WARNING_TRESPASS_FIRST', 'WARNING_TRESPASS_SECOND', 'WARNING_TRESPASS_THIRD'],
  heeded: 'HEED_WARNING',
  ignoredWarning: 'IGNORE_WARNING',
  ranFromWarning: 'RUN_AWAY_WHILE_WARNING',
  ranFromBribe: 'RUN_AWAY_WHILE_REBRIBE',
  fineReceived: 'RECEIVE_FINE',
  bribeReceived: 'RECEIVE_BRIBE',
  sweep: 'START_SECURITY_SWEEP',
  wary: 'GREET_WARY',
} as const;

/** How many numbered variants Fable has for each bark family (both voices have the same count). */
const BARK_VARIANTS: Record<string, number> = {
  SEEN_MINOR_CRIME: 2,
  SEEN_MODERATE_CRIME: 2,
  SEEN_SERIOUS_CRIME: 2,
  SEEN_CRIMINAL: 2,
  WARNING_WEAPON_OUT_FIRST: 2,
  WARNING_WEAPON_OUT_SECOND: 2,
  WARNING_WEAPON_OUT_THIRD: 2,
  WARNING_TRESPASS_FIRST: 2,
  WARNING_TRESPASS_SECOND: 2,
  WARNING_TRESPASS_THIRD: 2,
  HEED_WARNING: 2,
  IGNORE_WARNING: 2,
  RUN_AWAY_WHILE_WARNING: 2,
  RUN_AWAY_WHILE_REBRIBE: 2,
  RECEIVE_FINE: 1,
  RECEIVE_BRIBE: 2,
  START_SECURITY_SWEEP: 2,
  GREET_WARY: 5,
};

export function barkId(family: string, guardId: number, roll: number): string {
  const voice = guardId % 2 === 0 ? 'LEV1GRD1' : 'LEV2GRD1';
  const n = BARK_VARIANTS[family] ?? 1;
  const v = 1 + Math.min(n - 1, Math.floor(roll * n));
  return `TEXT_AI_GUARD_${family}_${String(v).padStart(2, '0')}_${voice}`;
}

/** Fine-screen and prompt text ids. */
export const TEXT = {
  payFor: 'TEXT_GUI_CRIME_PAY_FOR',
  totalFine: 'TEXT_GUI_CRIME_TOTAL_FINE',
  handOver: 'TEXT_GUI_CRIME_FINE_HAND_OVER',
  lackFunds: 'TEXT_GUI_CRIME_FINE_LACK_FUNDS',
  kickedOut: 'TEXT_GUI_CRIME_FINE_KICKED_OUT',
  kickedOutTrespass: 'TEXT_GUI_CRIME_KICKED_OUT_TRESPASS',
  kickedOutMurder: 'TEXT_GUI_CRIME_KICKED_OUT_MURDER',
  stillToPay: 'TEXT_GUI_CRIME_FINE_STILL_TO_PAY',
  forgivenIn: 'TEXT_GUI_CRIME_TIME_UNTIL_FORGIVEN',
  bribeOffer: 'TEXT_GUI_CRIME_BRIBE_OFFER',
  bribeLackFunds: 'TEXT_GUI_CRIME_BRIBE_LACK_FUNDS',
} as const;
