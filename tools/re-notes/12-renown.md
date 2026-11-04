# Hero renown

Read statically from Steam Fable: The Lost Chapters (no ASLR, so the addresses are fixed).
The mod logs the number it reads ("sync: renown in Fable is N"), to confirm in game.

## Where it lives

On the hero's stats component, `CTCHeroStats` (vtable `0x0124F70C`), not on the thing.
Its save code (vtable slot 1, `0x0057E2F1`) names every field it writes, which gives:

| Offset | Save name | Type |
| --- | --- | --- |
| +0x03C | Money | int |
| +0x070 | RenownLevel | int |
| +0x078 | RenownPointsInLevel | int |
| +0x07C | RenownTotal | int |

`0x0057D8D6` (`add [esi+0x7C], edi`) is where renown is added.

## Finding the component

Things keep their components in a vector of `{int type, component*}` pairs sorted by type:
begin at `thing+0x44`, end at `thing+0x48` (searched by `0x0040F020`). The stats component is
type 4: the script interface's GiveGold (slot +0x1F8, `0x00898F40`) finds it this way, checks
`thing+0x20` bit 0x10, then calls `0x0057B327`, which adds to +0x3C.

There is no renown getter on `CGameScriptInterface`, so the mod reads +0x7C directly
(`client-mod/src/game/thing.cpp`, `read_renown`) after checking the component's vtable.

## Online

Quests run on each player's own Fable, so the mod reports this total (`renown_report`).
The server (`server/src/heroes/renown.ts`) takes it once as an import, capped by
`FO_RENOWN_IMPORT_MAX`, and afterwards only raises its own number, by at most
`FO_RENOWN_DAILY_MAX` a day. Name tags show the server's number.
