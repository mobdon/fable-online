# Guards, crime and prison

The server owns the whole crime pipeline. The mod only reports what the player tried
(steal, draw a weapon, walk into private property, answer a guard). Guards are server-run
creatures. Crime records, fines, bribes, kick-outs and sentences live in the database, and
every gold movement goes through the ledger under the `crime` sink, so fines and bribes
leave the game.

Code: `server/src/law/` (`rules.ts` numbers and text ids, `law.ts` records and prison,
`guards.ts` the town watch per channel). Guards reuse the monster AI in
`server/src/world/ai/` with a `guard` profile and three orders: `pursue`, `approach`
(walk up and stand, for a fine) and `standDown`.

## Crimes

Kinds and their fine-screen lines come from Fable's own `TEXT_GUI_CRIME_*` entries. The
server sends text ids only; each client resolves them from its own `text.big`.

| Kind | Text id | Starting fine | Wanted level | Notes |
|---|---|---|---|---|
| `weapon_out` | `TEXT_GUI_CRIME_WEAPON_OUT` | 20 | 1 | Three guard warnings first. Petty |
| `trespass` | `TEXT_GUI_CRIME_TRESPASS` | 50 | 2 | Three warnings first. Petty. 10 min out of town |
| `vandalism` | `TEXT_GUI_CRIME_VANDALISM` | 50 + damage | 1 | Damage value capped at 500 |
| `lockpicking` | `TEXT_GUI_CRIME_LOCKPICKING` | 100 | 1 | |
| `pickpocket` | `TEXT_GUI_CRIME_PICK_POCKETS` | 150 + 2 x value | 1 | NPCs only |
| `theft` | `TEXT_GUI_CRIME_THEFT` | 2 x value, min 100 | 1 | |
| `assault` | `TEXT_GUI_CRIME_ASSAULT` | 300 | 2 | 10 min out of town |
| `gbh` | `TEXT_GUI_CRIME_GBH` | 800 | 2 | Half a victim's health in blows. 30 min out |
| `murder` | `TEXT_GUI_CRIME_MURDER` | 2,000 | 3 | Jail. No bribe |
| `murder_player` | `TEXT_GUI_CRIME_MURDER` | can't be paid | 4 | Bargate. No witness needed |

- A repeat of the same crime within 24 hours costs 1.5 times more.
- Unpaid totals of 1,500 and 5,000 raise the wanted level to at least 2 and 3.
- Petty crimes are forgiven after 60 minutes of **online** play. Nothing else is forgiven.
- Witnesses: a guard with a clear view records the crime at once and barks. A villager or
  trader who saw it tells the guards 4 seconds later, if they are still alive. Crimes
  against heroes who haven't flagged for PvP are always recorded (the server ran the fight).

## Guards

Each town (`rules.jurisdictions`) has a watch. What a guard does depends on the hero's
wanted level:

1. **Level 1:** walks up and asks for the fine (`law_prompt` kind `fine`), with a bribe offer.
2. **Level 2:** the same, and paying throws the hero out of town (`fine_kick`).
3. **Level 3+, or resisting:** every guard that sees the hero gives chase. A guard staying
   within 2.6 m for 1.2 s makes the arrest. A hero felled by a guard is arrested, not killed.

Resisting means walking away from a guard, refusing, ignoring the prompt for 20 seconds, or
hitting a guard. A criminal out of every guard's sight for 30 seconds is lost for now; the
record stays. A fine the hero can't pay is a trip to jail.

Barks are `TEXT_AI_GUARD_<family>_<NN>_<voice>` ids (voices `LEV1GRD1` and `LEV2GRD1`, one
per guard), so the client plays Fable's own lines and sounds.

## Bribes

`max(200, 1.5 x unpaid fines in that town)`, doubled for each bribe that real day. A bribe
clears level 1 and 2 crimes in one town and that town's guards ignore petty crimes for 5
minutes. It is always dearer than paying the fine. Murder, crimes against heroes and stolen
marks can't be bribed away.

## Prison

Sentences only count down while the hero is online, and a hero who logs off appears back in
the cell.

| | Bowerstone Jail | Bargate Prison |
|---|---|---|
| Sent for | wanted level 3, NPC murder, a fine the hero can't pay | level 4 (a murdered hero), or the fourth jail trip in a week |
| Time | 5 min per 1,000 gold of fines, 2 to 15 min | 20 min, doubling per repeat in a week, 2 h at most |
| Where | a private copy of `BowerstoneJail` | shared `PrisonCells_1..3`; the grounds can be walked |
| Leaving early | pay the fines plus 25% (`payout`) | the Warden's Office halves what's left, once, at 50 gold per minute halved |
| Release | `BowerstonePosh` | `PrisonPath` |

From the third Bargate trip in a week, the first half is served in the Torture Chamber.
The escape (`Law.escape`) is built but switched off (`open.bargateEscape`).

While wanted at level 2 or more, or in prison, shops and Cullis Gates are refused.

## Stealing

Stealing is a request: the server checks the caps, rolls the spotted chance and decides.

- Only from NPC shops and homes. Never from heroes or their property.
- Spotted chance: 25% base, +10% by day, +5% per steal in the last hour, -3% per stealth
  armour piece, +60% when a guard can see the hero, between 5% and 95%. The client's own
  "spotted" event adds 50%; it can never lower the chance.
- Unseen: the item is created with a 72-hour stolen mark. Marked items can't be sold to
  normal traders or traded to heroes. The fence buys them at 25% of value.
- Seen: theft on the record, and the item stays in the shop.
- 10 seconds between attempts, one of each item per shop per hero per day, and 2,000 gold
  of stolen value per hero per day (the fence takes the same at most).

At 25% spotted, a typical steal loses gold on average; the tests check it.

## Protocol

Client to server, `{ "t": "law", "a": ... }`:

| `a` | Fields | |
|---|---|---|
| `weapon` | `on` | Weapon drawn or sheathed |
| `trespass` | `zone` or `null` | Entered or left private property |
| `report` | `kind` (`lockpicking`, `vandalism`), `value` | Something the guards may have seen |
| `steal` | `shop`, `def`, `pickpocket`, `spotted`, `key` | A steal attempt |
| `answer` | `choice` (`pay`, `bribe`, `refuse`), `key` | Answer to a guard's prompt |
| `fence` | `item`, `key` | Sell a marked item to the fence |
| `payout`, `warden` | `key` | Pay out of jail, pay the warden |
| `status` | | Ask for `law_status` again |

Server to client: `law_status` (on join and on every change: wanted level, fine-screen
lines, minutes until petty crimes are forgiven, sentence, kick-outs; the mod writes it into
`CTCHeroRememberCrimes`), `law_prompt`, `law_bark`, `law_notice`, `law_wanted` (for name
tags, `HUD_CRIME_CRIMINAL`), `law_steal`, `law_fenced`, `law_released`, `law_warden`.

## Wiring still to do

- **Combat feed.** `ChannelLaw.onHarm(attacker, victim, damage, killed)` and
  `onHeroDowned(hero, killer)` need calling from the combat module's hit and death events,
  and guards need the AI's `isHostile` hook in that sim (done here for `NavSim`).
- **Places from the level files.** Kick-out targets for Oakvale, Knothole Glade and Hook
  Coast are placeholders, and moved heroes land at the region origin (snapped to ground)
  until spawn points and private-property zones are read from the `.lev` files.
- **Day and night** comes from the world clock once it exists; until then it counts as day.

## Open choices (switches in `rules.open`)

- Pickpocketing other heroes: off.
- A claimable bounty on hero murderers: off (an analytics event is ready).
- The Bargate escape: off.
- All fines, prices and times can be changed in `content/law.json`, which is merged over
  the defaults.
