# Town life: villagers, shops, day and night

The server runs villagers, shopkeepers, shops and the sky. No Fable.exe runs on the server;
clients draw what the server says.

## Data

`npm run extract-town -- "<Fable folder>"` (in `server/`) reads the operator's install and writes
`server/data/town-data.json` (never committed):

| What | Where it comes from |
| --- | --- |
| What each shop sells | `CShopDef` on `BUILDING_*` defs in `game.bin`: rows of `Type` (item), `Quantity`, `PriceMultiplier`, `MaxStock`, `RestockPeriod`. The field names hash to the ids in the file, so they are certain. |
| Who lives in a village | `CVillagePeopleDef` on `VILLAGE_*` defs: `MaleShopKeeper`, `MaleBarman`, `FemaleBarmaid`, `MaleWarehouseWorker`, `MaleUnemployed`, `FemaleUnemployed`, `Guard` |
| Where | `.tng` files in `FinalAlbion.wad`: `Village` things, `Building` things with `CTCShop` (40 shops) or `IsResidential` (homes), and hand-placed villager and trader `AICreature`s without a script name |

On the Steam install that is 38 shop types, 47 village types, 40 shops, 102 homes and 111 town
folk across the realm. Without the file the server uses a small made-up test village.

## Villagers and shopkeepers (`server/src/world/town/`)

`TownSim` wraps a channel's sim (combat or stub) and adds town folk to its world state. Each
channel has its own copy, like monsters.

- **Shopkeepers and barkeeps** stand at their shop (2 m in front of the building) and face heroes
  who come close. Shops stay open day and night.
- **Residents** (one per home, up to 12 a village) stroll within 18 m of the village by day, turn
  to face a hero who walks up, and go indoors at dusk (20:00). About one in five stay out late.
  Indoor villagers are not sent to clients. They come back out at dawn (06:00).
- **Fights**: a hero swinging or casting, or a monster fighting, within 10 m makes residents run
  away and shopkeepers cower. A shop doesn't trade while its keeper is cowering.
- `TownLife.witnesses(at, radius)` lists who could see a crime; `scare(at)` lets the crime system
  frighten people.

Entities: ids from 900,000,000, `def` is the Fable creature def, `health` is 1. Animations:
0 idle, 1 walk, 2 run, 11 cower, 12 greet, 13 working.

The client mod shows them through the entity sync (`docs/entity-sync.md`). The local Fable must
stop populating its villages (CTCVillage) the same way it stops creature generators, or there
will be two of everyone. Quest people (placed with a script name) stay local.

## Shops (`server/src/economy/shops.ts`)

One server shop per Fable shop building, id `<region>:<building UID>`. Prices are the item's
base value times Fable's markup, never below base value and at most 3x. Vendors pay back 25% of
base value, so buying and selling back always loses gold. Stock is shared by every channel of
the realm and refills to `MaxStock` every `RestockPeriod` game days. Hand-written shops in
`content/shops.json` override generated ones with the same id.

Game socket messages (client to server):

```
{"t":"shop_open","shop":"<building UID>"}
{"t":"shop_buy","shop":"<uid>","def":"OBJECT_IRON_LONGSWORD","qty":1,"key":"<8-64 chars, unique per purchase>"}
{"t":"shop_sell","shop":"<uid>","item":"<item id>","key":"<unique>"}
```

Server to client: `shop` (items with price and stock left, what the shop would pay for each
inventory item, gold) or `shop_result` (`ok`, `gold`, and `item_id`/`price` or `earned`, or
`error`: `too_far`, `shop_closed`, `out_of_stock`, `insufficient_funds`, ...). The server checks the
hero's position itself. A resent `key` never buys twice.

Client side, still to build: when Fable opens its own trade screen for a shop building, send
`shop_open` with that building's UID, show the server's prices and stock, and turn the screen's
buy and sell into `shop_buy` / `shop_sell` instead of Fable's own gold change.

## Day, night and weather (`server/src/world/clock.ts`)

One clock for the realm, a pure function of wall time: a 48-minute day by default
(`FO_DAY_MINUTES`). Weather rolls every 12 minutes (`FO_WEATHER_MINUTES`) per climate zone from a
seeded hash (`FO_WEATHER_SEED`), so every channel and every restart agree:

| Climate | Regions | Weather |
| --- | --- | --- |
| indoor | caves, cellars, interiors, chapels | none |
| snow | Snowspire, Northern Wastes | cloudy, snow, blizzard |
| coast | Hook Coast, Lost Bay | clear, cloudy, fog, rain, storm |
| gloom | Darkwood, Barrow Fields, Witchwood, bandit camps | cloudy, fog, rain, storm |
| temperate | everything else | mostly clear, some cloud, rain and fog |

The server sends `{"t":"sky", day_fraction, day_ms, night, climate, weather, intensity, previous,
since, until, fade_ms, server_time}` on entering a region, when the weather or day/night changes,
and every 60 s.

The mod writes `day_fraction` into Fable's time manager (addresses in
`tools/re-notes/08-world-time.md`; the mod side lands on top of the entity sync). Applying `weather` in the game is not mapped yet.
