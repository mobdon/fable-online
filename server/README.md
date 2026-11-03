# Fable Online server

The authoritative game server: accounts, heroes, the gold and item ledger, and the region
channels every player is simulated in. TypeScript on Node 22, one dependency (`ws`).

```
cd server
npm install
npm run dev          # http://localhost:7777, game socket ws://localhost:7777/play
npm test             # 19 tests: economy invariants, accounts, channels, end-to-end join
npm run bots -- 1000 20   # 1,000 bots over real WebSockets for 20 s
```

Needs Node 22.13 or newer (uses the built-in `node:sqlite`).

## The rules it enforces

**The server owns everything.** A client can send exactly four things: `hello` (a one-time
join ticket), `input` (stick, buttons, camera yaw), `chat`, and `ping`. It never sends a
position, health, gold or an item. The stick is clamped to length 1, input `seq` must go up,
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
- Player trades execute both sides in one transaction, against the exact offer both players
  accepted. Changing an offer clears both accepts.
- Resurrection Phials (and anything marked untradeable) can't be bought, sold or traded.
- `economy.audit()` re-derives every balance from the ledger and every stack from its
  history. The server refuses to start if it fails. `GET /admin/v1/economy/audit` runs it live.

**Region channels that autoscale** (`src/world/director.ts`). One channel is one copy of one
Albion region, and in production one server-mode Fable.exe. A graduated hero joins the
fullest channel of their region under the soft cap (48). When all are at the soft cap a new
channel opens, up to the global budget (64 channels). Past that, channels overflow to the hard
cap (64), then the region reports full. Empty channels close after 2 minutes. Childhood and
Guild training always get a private solo instance. Region changes only happen through a real
exit (checked against the region graph) or a Cullis Gate, never on client request.

## Layout

| Path | What |
| --- | --- |
| `src/config.ts` | Every tunable, overridable by `FO_*` env vars |
| `src/db/` | SQLite (dev) with migrations. Postgres goes behind the same repositories later |
| `src/accounts/` | scrypt passwords, access + rotating refresh tokens, one-time join tickets |
| `src/heroes/` | Heroes, life stages (childhood, guild_training, world), PvP unlock flag |
| `src/economy/` | Gold ledger, items, vendor prices, trade windows, audit |
| `src/world/` | Region table, channels, the director (placement and scaling), simulation interface |
| `src/world/nav/` | Walkable ground per region from Fable's own level files: A*, ground height, snapping ([docs/navigation.md](../docs/navigation.md)) |
| `src/world/ai/` | Server-run monsters: wander, notice, chase, attack, flee, leash, threat ([docs/creature-sync.md](../docs/creature-sync.md) for the client side) |
| `src/world/nav-sim.ts` | `NavSim`: heroes held to the ground plus AI creatures, used when `FO_NAV_DIR` is set |
| `src/net/` | HTTP launcher API, game WebSocket, protocol validation |
| `src/analytics/` | Typed game events written as JSON lines to `data/events.jsonl` |

## HTTP API

Launcher contract (`/launcher/v1`): `GET status`, `GET news`, `GET manifest?channel=live`,
`POST register` (logs straight in, same reply as login), `POST login`, `POST refresh`, `POST logout`, `GET heroes`, `POST heroes`,
`POST join {region, client_version, hero_id?}` which returns `{ticket, host, port}`. The mod
sends the ticket as `Hello.token`; it works once, for 60 seconds. News and manifests are read
from `content/news.json` and `content/manifest-<channel>.json`.

Admin (`/admin/v1`, admin accounts only): `GET stats`, `GET economy/audit`.

## What's a stand-in for now

- **`StubSim`** walks heroes around at run speed with no ground. Regions with an extracted
  navmesh (`node tools/extractor/navmesh.mjs <install>`, then `FO_NAV_DIR=../shared/defs/nav`)
  run on `NavSim` instead: heroes stay on walkable ground and monsters are run by the
  server's AI. Combat there is `StubCombat` (flat damage) until the combat module lands.
- **JSON frames.** About 45 KB/s per player in a full channel at 20 Hz. Moving to protobuf
  binary with delta updates and interest management is the next network step.
- **SQLite.** Fine for one machine and tests. Production moves to Postgres with SERIALIZABLE
  transactions, and the director to a separate process per region host.
- **Region graph** comes from `shared/defs/regions.json`, which you generate from your own
  install with `tools/extractor/regions.mjs`. Without it the server uses a small built-in map.
