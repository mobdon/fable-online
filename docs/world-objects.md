# Doors, loot and region changes

The server decides who opens or takes what, and when a hero moves from one region to the
next. Each player's Fable still draws the doors and chests and loads the regions, so the
mod reports what the hero tries to use and then makes the local game follow the
server's answer.

## Where the data comes from

`tools/extractor/things.mjs` reads the player's own install and writes
`shared/defs/world-things.json` (git-ignored, never committed):

```
node tools/extractor/things.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
# 398 levels: 139 exits (129 linked), 187 entrances, 173 doors, 118 chests, 218 containers,
# 79 pickups, 60 digging spots, 14 demon doors, 54 teleport doors
```

Start the server with `FO_THINGS` pointing at it (the default is
`../shared/defs/world-things.json`). Without the file the server runs with no doors,
loot or exits.

### Files

- `FinalAlbion.wad` holds one `.tng` per level: plain-text things, one
  `NewThing <kind>; ... EndThing;` block each, grouped into
  `XXXSectionStart <name>; ... XXXSectionEnd;`. Section `NULL` is the base level; any
  other section belongs to a quest and only exists while that quest runs.
- `FinalAlbion.gtg` holds global things, one `NEWMAP <map id>` block per map (the id is
  the `NewMap` number in the `.wld`). The region entrance points are here and in the `.tng`.
- `FinalAlbion.wld` lists maps (`NewMap`: `MapX`, `MapY`, `LevelScriptName`, `MapUID`) and
  regions (`NewRegion`: `RegionName`, one `ContainsMap` per level in the region).

Each thing has a `DefinitionType` and a list of components (`StartCTCxxx; ... EndCTCxxx;`).
The extractor goes by component, not by name:

| Component | What it is | Fields used |
| --- | --- | --- |
| `CTCDRegionExit` | region exit | `Active`, `Radius`, `EntranceConnectedToUID` |
| `CTCDRegionEntrance` | where a hero arrives | position, facing |
| `CTCDoor` | door that swings | `Open`, `DoorTriggerType`, owner from `CTCOwnedEntity` |
| `CTCChest` | chest | `ContainerContents[n]`, `ChestOpen`; `OBJECT_SILVERKEY_CHEST_<n>` needs n keys |
| `CTCSearchableContainer` | cupboard, bookshelf | contents from `CTCContainerRewardHero` |
| `CTCDiggingSpot` | digging spot | contents from `CTCContainerRewardHero` |
| `CTCInventoryItem` | item lying in the world | `CTCStockItem`: `ForSale`, `Stealable`, `Price` |
| `CTCActionUseScriptedHook` with `TeleportToRegionEntrance TRUE` | door that loads another region | `EntranceConnectedToUID` |
| `CREATURE_DEMON_DOOR_FACE_*` | demon door | `ScriptName` (the riddle) |

### Thing ids and links between levels

Thing UIDs are only unique inside one level: 3,642 UIDs repeat between `.tng` files. The
server calls a thing `<LevelScriptName>:<uid & 0xFFFFFFFF>`, for example
`LookoutPoint:112`.

`EntranceConnectedToUID` points into another level, so it carries the level too:
`(MapUID << 40) | uid within that level`. For example `0x137D3B0000000008` on a Lookout
Point exit is MapUID 1277243 (`Greatwood_1`), entrance 8. With that rule all 129 exits
outside the developer test level link to an entrance, and every link agrees with
`FinalAlbion_StartingRegionGraph.txt`. Every demon door stands within 5 m of the exit
into its `DemonDoor_*` region.

### Positions

A region can hold several levels (Lookout Point holds `BowerstoneBridge`, `LookoutPoint`
and `GuildExterior`). The server keeps one frame per region: the frame of the level whose
script matches the region name, or else its first level. Things in the other levels are
moved by the difference in `MapX`/`MapY`. World position = `MapX + x`, `MapY + y`, `z`;
the creatures' `InitialPosX` in the `.tng` agree (2067.01 = 2048 + 19.01 on
`BanditCampPath_1`).

## What the server does

`server/src/world/things/objects.ts` (`WorldObjects`):

| Thing | Rule |
| --- | --- |
| Chest | Each hero once, ever. Silver key chests need the keys in the hero's inventory; the keys are not used up and can't be traded. |
| Digging spot | Each hero once, ever, with a spade. |
| Cupboard, bookshelf | Each hero once a day. |
| Unique item on the ground (silver key, quest item, card, book, trophy) | Each hero once, ever, in their own copy of the world. |
| Common item on the ground (potion, food) | Shared by the channel: first come, gone for everyone, back after 10 minutes. Each hero once per 10-minute window. |
| Door | Shared by the channel. Swings shut 20 s after being opened. Prison doors stay locked. |
| Demon door | The riddle runs in the hero's own game, like every quest. The client reports `opened`; the server checks the hero is within 10 m, records it per hero, and only then lets that hero through the exit behind it. The reward chests behind it are ordinary once-per-hero chests. |
| Door that loads another region | Moves the hero to the linked entrance. |
| Region exit | Walking into it (server position) moves the hero to the linked entrance. |

No duping: every chest, spot, container and pickup goes through
`Economy.claimLoot` with a claim key such as `chest:<hero>:<thing>`. The key is unique in
the ledger, so a doubled click, a resend after lag, a reconnect or the same chest in
another channel is a replay that creates nothing. Gold is minted with reason `loot`
(capped at 5,000 per claim) and items get new unique ids, all in one transaction.

Every use is checked against the hero's position on the server (3 m across, 3 m up or
down; doors 4 m), never the client's.

Loot rules (`loot.ts`):

- `OBJECT_GOLD_<n>` and `OBJECT_GOLDBAG_..._<n>` are n gold.
- `OBJECT_DYNAMIC_POTION_DUMMY` and `OBJECT_DYNAMIC_RESURRECTION_DUMMY` are rolled by
  the server, seeded by the claim, so a retry rolls the same. The phial slot gives a phial
  one time in four.
- The platinum hero set is never created from world loot. The retail data has the whole
  set in one chest in the Northern Wastes (`NorthernWastes2:4191`); the server hands out
  the rest of that chest and reports the blocked items.
- Items the starter catalog doesn't know are added as "vendors won't buy or sell" until
  real prices come from the game data, so loot can't be turned into gold yet.
- Taking a `Stealable` item emits a `theft` event for the crime system.

Other systems can stop a hero using a thing by adding a `Gate` (for example a jailed hero
and the prison doors).

### Region changes

Each server frame, after the sims step, every hero in a region with exits is checked
against them:

1. A hero who has just arrived is "disarmed" and must step clear of every exit
   (1.25 x its radius) first, so arriving on top of the exit back does not bounce them.
2. Walking inside an exit's radius (and within reach in height) hands the hero to the
   linked region through `Director.transfer`: the hero leaves the old channel, joins the
   fullest channel of the next region with room (or their own instance before
   graduation), and the new region and position are saved.
3. The hero is frozen in the new channel (inputs ignored) until the client sends
   `region_ready`, or for at most 30 s.

Exits that only exist in quest sections are left to the client's quest for now.

## Protocol

Client to server:

| Message | Meaning |
| --- | --- |
| `{"t":"interact","thing":"LookoutPoint:112","action":"use"}` | The hero used a thing. `action` is `use` (default), `open`, `close`, or `opened` (demon doors). |
| `{"t":"region_ready","region":"GreatwoodEntrance"}` | The local game finished loading the region it was sent to. |

Server to client:

| Message | Meaning |
| --- | --- |
| `{"t":"enter_region","region":...,"x","y","z","from","via":"exit"\|"door","entrance":"Greatwood_1:8","yaw":...,"loading":true}` | Load this region and put the hero at this entrance. `from`/`via`/`entrance`/`loading` only on a region change. |
| `{"t":"objects","region":...,"doors":[[id,open]],"gone":[ids],"opened":[ids]}` | Sent after every `enter_region`: doors not in their default state, items gone for this hero, chests and demon doors this hero already opened. |
| `{"t":"object","id":...,"open":true}` / `{"gone":true}` / `{"opened":true}` | One thing changed. |
| `{"t":"loot","thing":...,"gold":500,"items":[{"item_id","def"}],"blocked":[],"again":false}` | What the hero got. `again` means they already had it and nothing new was made. |
| `{"t":"error","error":"too_far"\|"need_keys"\|"gone"\|"locked"\|"demon_door_closed"\|...,"thing":...}` | Refused. |

## The client side

Fable.exe has no ASLR, so these vtables (from `tools/re/rtti_map.py`) are fixed:

| Class | vtable | Use |
| --- | --- | --- |
| `CTCDRegionExit` | 0x0126AB54 (30 slots) | the exit trigger |
| `CTCDRegionEntrance` | 0x0123BA74 | arrival points |
| `CTCDoor` | 0x01266DE4 | doors |
| `CTCChest` | 0x0126957C | chests (derives from `CTCContainerRewardHero`) |
| `CTCContainerRewardHero` | 0x01269424 | hands container contents to the hero |
| `CTCSearchableContainer` | 0x012431F4 | cupboards, bookshelves |
| `CTCDiggingSpot` | 0x0127017C | digging spots |
| `CTCInventoryItem` | 0x01240004 | items lying in the world |
| `CTCActionUseChest` | 0x012400F4 | the hero's "open chest" action |
| `CTCActionUseScriptedHook` | 0x0126664C | doors that load another region |
| `NEntityEvents::CEventOpenedChest` | 0x01274258 | event fired when a chest opens |

The jobs in the mod, in order:

1. **Thing ids.** Read a thing's UID (a `CThing` field, offset still to map) and the
   current level script name, and build `<Level>:<uid & 0xFFFFFFFF>`. Check against a
   chest on Lookout Point before anything else depends on it.
2. **Region exits.** In online mode, stop `CTCDRegionExit` starting a load by itself
   (its per-frame slot; to map). The server sees the hero walk into the exit and sends
   `enter_region` with `via` and `entrance`; the mod then loads that region and places the
   hero at that entrance through Fable's own region-entrance teleport (the
   `TeleportToRegionEntrance` path that `CTCActionUseScriptedHook` already uses), and sends
   `region_ready` when the load finishes. A shut demon door exit gets `demon_door_closed`
   and nothing loads.
3. **Chests, containers, digging spots.** Let the open animation play, but stop
   `CTCContainerRewardHero` adding the contents to the local inventory (its give slot; to
   map). Send `interact` instead. The server's `loot` reply is what the hero really got;
   the inventory sync puts it in the local inventory. A chest listed in `opened` is set
   open on region load.
4. **Doors.** When the hero uses a door, send `interact` and don't open it locally. On
   `object` with `open`, call the door's own open or close so everyone sees it swing.
5. **Items on the ground.** On pick-up, send `interact` instead of taking it. On `loot`
   hide the local item; on `object` `gone` hide it for everyone; on `gone:false` show it.
6. **Demon doors.** The face's riddle script runs locally. When it opens, send
   `interact` with `opened`. Until the server agrees, the exit behind it does nothing.

### What the mod does today

Built on the entity sync layer (`docs/entity-sync.md`):

- `client-mod/src/world/objects.{h,cpp}` (pure logic, `tests/objects_test.cpp`) keeps the
  server's answers: door states, items gone, chests and demon doors opened, loot received
  and refusals. It builds `interact` and `region_ready` frames, sends a request for a
  thing at most once a second while it waits for the answer, refuses requests while a
  region is loading, and forgets the old region on every region change.
- `client-mod/src/sync.cpp` feeds it every frame. After an `enter_region` with
  `loading`, it waits for the local game's own load to finish (the hero exists again and
  is in a different area) and sends `region_ready`, or sends it after 25 s so the hero is
  never stuck. Until job 2 is mapped, the local game loads the next region itself when the
  hero walks through the exit, at the same moment the server moves them.
- `client-mod/src/game/things.{h,cpp}` is where the changes reach Fable. Finding the local
  thing for a server id is not mapped yet, so each change is logged
  (`things: open door LookoutPoint:...`) instead of applied.

Not done yet: jobs 1 and 3 to 6 above (the slots and offsets in the running game), and the
inventory sync the loot relies on.
