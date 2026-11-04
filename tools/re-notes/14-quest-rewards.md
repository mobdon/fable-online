# Quest completion and quest rewards

Found by disassembly of the Steam Fable.exe (no ASLR, image base 0x400000). Nothing below has
been watched in a running game yet.

## Where Fable completes a quest

- `CQuestManager` singleton at `[0x013B89FC]` (vtable 0x01239464).
- `0x004B1D30` `CQuestManager::SetQuestAsCompleted(const CCharString& quest, bool, bool, bool)`,
  `__thiscall`, `ret 0x10`. When the first bool is set it opens the quest-completion screen
  (`CTCQuestCompletionUI` mode 1). Called from two places only:
  - `0x00892F9F`, inside `0x00892F80`, which is `CGameScriptInterface` slot `+0x480` (what quest
    scripts call),
  - `0x0061ACB3`.
- `0x004B0C80` `CQuestManager::FindQuestCard(const CCharString& quest) -> CThing*`,
  `__thiscall`. Walks the card list at `mgr+0x5C`; for each card thing it takes the component
  of type `0x6C` (`CTCQuestCard`) and compares that component's `+0x28` script name.
- `CCharString` is a pointer to `{ char* text; int length; ... }`.

## CTCQuestCard (vtable 0x01264C44, constructor 0x007026A0)

| Offset | What |
| --- | --- |
| +0x04 | owner thing (the quest card object; its def index at thing `+0x8C`) |
| +0x0C | the card's runtime `CQuestCardDef` |
| +0x10 | byte, set to 1 when the quest is completed |
| +0x28 | `CCharString` quest script name (e.g. `Q_BanditCamp`) |
| +0x2C | gold reward, copied from def `+0x48` (`GoldReward`) when the card is made |
| +0x30 | renown reward, copied from def `+0x44` (`RenownReward`) |

The completion screen copies `+0x2C` and `+0x30` into its own `+0x58` and `+0x5C`. Which of the
two is gold and which renown is inferred from the def's field order (RenownReward comes before
GoldReward in game.bin), not yet seen in game: the first claim's log line settles it.

## CTCQuestCompletionUI (vtable 0x0125589C, constructor 0x005E4030)

- `0x005E4740` `Begin(int mode, const CCharString& quest, ..., bool)`, `ret 0x14`.
- `0x005E4430` `Start(int mode, CTCQuestCard*, bool)`. Mode 0 start screen, 1 complete,
  2 failed, 3 death (jump table at 0x005E78C4 picks `PC_QUEST_COMPLETION_MENU_*`).

## Boasts

The boast amounts are script constants in `data/CompiledDefs/script.bin` (same container as
game.bin; one `CScriptDef`, `SCRIPT_DEF`). Names follow `<Prefix><Boast>BoastReward` /
`...BoastCost` (e.g. `OFGoodNoDamageBoastReward` = 400, `ArenaNoDamageBoastReward` = 10000).
`server/src/data/boasts.ts` maps each prefix to its quest card. Some boast names in Fable.exe
(e.g. `BS_KilledLeaderBoastReward`) are not in script.bin and are not counted.

## What the mod does

`client-mod/src/game/quests.cpp` points both calls to `0x004B1D30` at its own function. Before
Fable's code runs it looks up the card (`0x004B0C80`), reads the card's def name and
`+0x2C`/`+0x30`, then calls Fable's function and queues a `quest_complete` claim. The server
pays at most card + boasts, once per hero.

To check in game: finish a quest (the first Guild training card or Wasp Menace) and look for
`quests: Fable completed ...` in fable_online.log, then `quests: server paid ...`.
