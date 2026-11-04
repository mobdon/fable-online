# Fable Online server

The authoritative game server: accounts, heroes, the gold and item ledger, and the region
channels every player is simulated in. TypeScript on Node 22, one dependency (`ws`).

```
cd server
npm install
npm run dev          # http://localhost:7777, game socket ws://localhost:7777/play
npm test             # economy invariants, accounts, channels, combat, spawns, loot, quests, end-to-end join
npm run bots -- 1000 20   # 1,000 bots over real WebSockets for 20 s
npm run walkers -- --near Mob   # bot players walking around a hero on a running server
npm run admin -- list           # heroes and where they are
```

To host a test server on a Windows PC, use `ops/local-host/` (README there).

### Game data from your own install

The server simulates combat with Fable's own numbers, read from your install at setup time.
Nothing from the game is committed or shipped; the outputs below are gitignored.

```
npm run extract -- "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
node ../tools/extractor/regions.mjs "<same folder>"   # shared/defs/regions.json
node ../tools/extractor/spawns.mjs  "<same folder>"   # shared/defs/spawns.json
node ../tools/extractor/quests.mjs  "<same folder>"   # shared/defs/quests.json
npm run extract-town -- "<same folder>"               # data/town-data.json: shops, stock, villages
```

`npm run extract-town` reads shop stock and village people from `game.bin` and where villages,
shops and homes are from `FinalAlbion.wad`. See `docs/town-life.md` for villagers, shops and the
day, night and weather clock.

`npm run extract` decodes `data/CompiledDefs/game.bin` (format notes in
`tools/re-notes/07-game-bin.md`) into `data/game-data.json`: weapon type, class and damage,
item base values, hero clothing coverage and armour class, creature health, faction, weapon,
experience and reward values, creature generation families, quest card rewards, and the
per-level spell tables. Without these files the server runs on a small made-up fallback set.

Needs Node 22.13 or newer (uses the built-in `node:sqlite`).

## The rules it enforces

**The server owns everything.** A client can send exactly these things: `hello` (a one-time
join ticket or resume token), `input` (stick, buttons, camera yaw), `chat`, `interact` (the id
of a door, chest or item it wants to use), `region_ready`, `quest_complete` (a claim the server
checks, below), `hero_pos` (where its Fable put the hero, accepted only if the hero could have
got there: `docs/connection.md`), and `ping`. It never sends health, gold or an item. The stick is clamped to length 1, input `seq` must go up,
inputs are rate limited, and junk frames disconnect. The hero's region and position come
from the database on join and are written back by the server.

**No money loopholes** (`src/economy/economy.ts`):

- Double-entry gold. Gold enters only from `system:mint` with a whitelisted reason and a
  per-action cap (loot 5,000, quest 50,000, ...), and leaves only into `system:sink`. All
  wallets always sum to exactly 0, so total gold in the world is always known.
- Hero wallets can't go negative (DB constraint and guarded update). Amounts are whole
  numbers, 1 to 1,000,000,000.
- Every action carries an idempotency key. A lagged retry or double-click returns the first
  result instead of paying twice.
- Every item instance has its own random 128-bit ID that is never reused, plus a full history.
  Items are moved, never copied. Split and merge conserve quantity.
- Prices live in the server's item catalog. It refuses to load if any item sells back for
  more than half its buy price, so buy-then-sell can never make gold.
- Player trades execute both sides in one transaction, against the exact offer version both
  players accepted. Changing an offer clears both accepts, and an accept is refused for 2 s
  after a change. Inside the swap the server re-checks ownership, gold, stolen marks, staff
  gear, prison and wanted level; if anything fails nothing moves and the window stays open.
  Both heroes must be in the same channel within 10 m, and walking away, logging out or 5
  quiet minutes closes the window. Part of a stack is split off into a new item ID. Nothing
  sits in escrow, so there is nothing to roll back. Socket messages: `trade_*` in
  shared/protocol/messages.proto.
- Resurrection Phials (and anything marked untradeable) can't be bought, sold or traded.
- `economy.audit()` re-derives every balance from the ledger and every stack from its
  history. The server refuses to start if it fails. `GET /admin/v1/economy/audit` runs it live.

**Combat is decided on the server** (`src/combat/`, `src/world/combat-sim.ts`). Input only
carries button bits (attack, block, cast + spell slot). The server picks the target in the
attack cone, checks reach and facing, applies cooldowns, blocks (front only; flourishes
break them), knockdowns with stun immunity afterwards (no stun-lock), armour
(`A / (A + K)`, capped at 60%, arrows and spells only partly reduced), PvP scaling, Will
costs and spell cooldowns, and deaths. A hero who dies uses a Resurrection Phial if they
carry one; otherwise they respawn at their entry point and lose a small, capped share of
their gold. All numbers live in `src/combat/rules.ts`.

**Spawns, loot and shops** (`src/world/spawns.ts`, `src/economy/loot.ts`, `src/economy/shops.ts`).
Each channel spawns the hostile creatures (monsters, bandits) the level files place, and keeps
creature generators topped up from their families. Kills pay experience (with Fable's combat
multiplier) and loot to the hero who did the most damage; loot gold is minted with reason
`loot` under its cap, keyed per creature so a kill can't pay twice. Shops only decide stock;
prices are the catalog's (base value to buy, 25% back on sale). Platinum armour is staff-only:
it can't be minted for players, sold, dropped, traded or worn by a non-staff hero, and every
attempt is logged as `staff_item_blocked`.

**Quests run on the client, so their rewards are capped** (`src/economy/quests.ts`). A
`quest_complete` claim names a quest card (rewards from game.bin) or a quest script name
(capped at `FO_QUEST_DEFAULT_MAX_GOLD`). It pays only if the quest is real, hasn't been paid
before (or its repeat interval passed), gold and renown are within the card, every item is
one the card gives, and the hero is under the daily quest-gold cap. Anything else pays
nothing and is logged as `quest_reward_rejected`. Overrides go in `content/quest-rewards.json`.

**Region channels that autoscale** (`src/world/director.ts`). One channel is one copy of one
Albion region, and in production one server-mode Fable.exe. A graduated hero joins the
fullest channel of their region under the soft cap (48). When all are at the soft cap a new
channel opens, up to the global budget (64 channels). Past that, channels overflow to the hard
cap (64), then the region reports full. Empty channels close after 2 minutes. Childhood and
Guild training always get a private solo instance. Region changes only happen through a real
exit (checked against the region graph), a door that leads to another region, or a Cullis
Gate, never on client request. The server sees the hero walk into the exit, moves them to
the linked entrance in the next region and holds them there until their game has loaded it.

**Doors, chests and loot** (`src/world/things/`, [docs/world-objects.md](../docs/world-objects.md)).
Each chest and digging spot pays each hero once, ever; common items on the ground are first
come, first served per channel and come back after 10 minutes; doors are shared by the
channel. Every claim is one ledger transaction with a unique claim key, so it can't be
doubled, and every use is checked against the hero's server position. The staff-only
platinum set never drops.

## Layout

| Path | What |
| --- | --- |
| `src/config.ts` | Every tunable, overridable by `FO_*` env vars |
| `src/db/` | SQLite (dev) with migrations. Postgres goes behind the same repositories later |
| `src/accounts/` | scrypt passwords, access + rotating refresh tokens, one-time join tickets |
| `src/heroes/` | Heroes, life stages (childhood, guild_training, world), PvP unlock flag |
| `src/economy/` | Gold ledger, items, vendor prices, trade windows, audit |
| `src/world/nav/` | Walkable ground per region from Fable's own level files: A*, ground height, snapping ([docs/navigation.md](../docs/navigation.md)) |
| `src/world/ai/` | Server-run monsters: wander, notice, chase, attack, flee, leash, threat ([docs/creature-sync.md](../docs/creature-sync.md) for the client side) |
| `src/world/things/` | Doors, chests, containers, pickups, demon doors and region exits from the level files (`tools/extractor/things.mjs`, `FO_THINGS`) |
| `src/world/nav-sim.ts` | `NavSim`: heroes held to the ground plus AI creatures, used when `FO_NAV_DIR` is set |
| `src/world/` | Region table, channels, the director (placement and scaling), combat sim, spawns, shared entity model, world clock and weather |
| `src/world/town/` | Villagers and shopkeepers, shops built from Fable's shop defs |
| `src/combat/` | Combat rules and balance table, hero and creature loadouts |
| `src/data/` | game.bin / names.bin reader and the extractor |
| `src/law/` | Crime records, fines, bribes, kick-outs, jail and Bargate sentences, stealing rules, and the town watch that runs guards ([docs/law.md](../docs/law.md)) |
| `src/net/` | HTTP launcher API, game WebSocket, protocol validation |
| `src/analytics/` | Typed game events (JSON lines in `data/events.jsonl`), and the bridge that feeds them to Prometheus metrics (`:9464/metrics`) and the economy watchdog ([README](src/analytics/README.md)) |
| `src/chat/` | Realm chat: channels, whispers, moderation, flood control and name tags, on TCP 7781 |

## HTTP API

Launcher contract (`/launcher/v1`): `GET status`, `GET news`, `GET manifest?channel=live`,
`POST register` (logs straight in, same reply as login), `POST login`, `POST refresh`, `POST logout`, `GET heroes`, `POST heroes`,
`POST join {region, client_version, hero_id?}` which returns `{ticket, host, port, chat: {host, port, token}}`. The mod
sends the ticket as `Hello.token`; it works once, for 60 seconds. News and manifests are read
from `content/news.json` and `content/manifest-<channel>.json`.

Admin (`/admin/v1`, admin accounts only): `GET stats`, `GET economy/audit`.

## What's a stand-in for now

- **`StubSim`** walks heroes around at run speed with no ground. Regions with an extracted
  navmesh (`node tools/extractor/navmesh.mjs <install>`, then `FO_NAV_DIR=../shared/defs/nav`)
  run on `NavSim` instead: heroes stay on walkable ground and monsters are run by the
  server's AI, when `FO_COMBAT=0`. With combat on (the default), `CombatSim` runs and the
  monster AI drives its creatures through `ChannelAi` (`src/world/ai/combat-ai.ts`), using
  the region's navmesh when `FO_NAV_DIR` is set and straight-line movement otherwise.
- **Hero movement** in `CombatSim` is still kinematic (run speed along the stick, no walls).
  `FO_COMBAT=0` falls back to `NavSim` or the movement-only `StubSim`.
- **Armour class values and hero levels.** Armour ratings come from the armour balance
  design, not game.bin (no per-piece rating was found there). Strength, Skill and Will levels
  aren't stored yet, so heroes fight at level 0 plus their gear.
- **Unverified fields.** Creature `reward2` (used for loot gold) and `attackScale` are named
  from how their values behave across defs, not from a field name. See the re-notes.
- **JSON frames.** About 45 KB/s per player in a full channel at 20 Hz. Moving to protobuf
  binary with delta updates and interest management is the next network step.
- **SQLite.** Fine for one machine and tests. Production moves to Postgres with SERIALIZABLE
  transactions, and the director to a separate process per region host.
- **Region graph** comes from `shared/defs/regions.json`, which you generate from your own
  install with `tools/extractor/regions.mjs`. Without it the server uses a small built-in map.

## Realm chat

Chat channels, whispers, moderation, flood control and name tags (`src/chat/`), run by the game
server on its own TCP port (`FO_CHAT_PORT`, 7781). Protocol: `../shared/protocol/chat.md`.
`RealmChat` (`src/chat/realm-chat.ts`) signs the chat login token returned by `/join` (there is no
login by name), and five times a second copies each online hero's channel, Fable area, position,
renown and PvP flag from the world into the hub, so the client's own position reports are
ignored. Chat events (logins, message counts, reports, moderation) go to the game event log and
the analytics. Hook left for the party system: `ChatHub.setParty()`.
