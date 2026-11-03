# Entity sync: showing what the server sees

Other players and the server's monsters are the same problem. In both cases the server
owns an entity and each player's own copy of Fable has to draw it. One layer in the mod
does both, so stand-in heroes and server creatures share their code, their smoothing and
their limits.

```
server region channel ──frames──▶ world_client ──▶ EntityTable ──poses──▶ PuppetPool ──▶ Fable
   (authoritative)                 (WebSocket)      (smoothing)          (engine calls)
```

## What the server sends

`server/src/world/channel.ts` builds one frame per tick (20 a second by default). Each
entity carries its id, position in the region's local metres, facing, animation, health,
and either a hero's name and renown or a creature's Fable def name
(`shared/protocol/messages.proto`). A player is only sent the entities within
`FO_INTEREST_RADIUS` metres of their own hero, because a busy region can hold hundreds of
creatures and the mod can only show what is near enough to see. An entity that stops
appearing in frames is gone: there is no separate removal message to lose.

## What the mod does with them

`client-mod/src/world/` decodes frames and keeps a short history of each entity, then
answers one question thirty times a second: where should this be drawn now? It answers it
for 100 ms ago, interpolating between the two frames around that time, so a late or
dropped frame shows up as smooth movement rather than a jump. Nothing is extrapolated
past the newest frame: an entity that stops arriving stands still instead of sliding on
through a wall. An entity missing for two seconds is removed; a frame whose tick number
has gone backwards is dropped.

That part has no Windows or game dependencies and is tested on its own:
`client-mod/tests/run.sh`.

## Puppets

`client-mod/src/game/puppet.cpp` gives each remote entity a local creature. Creating one
goes through the same `CreateNear` the game's own quests use
(`tools/re-notes/05-spawn-and-scripted-control.md`), with a unique script name, and then:

- the position and facing are written straight into the creature's physics component every
  tick, which is what actually decides where it stands;
- its actions are dropped once a second, so Fable's own AI stops steering it;
- it is made unattackable and undamageable, because the server decides every hit. The
  local game must never show a result the server has not agreed to.
- when its entity goes, it is hidden and kept, and the next entity with the same def gets
  it instead of a new spawn.

Another player's hero stands in as the creature def in `FABLE_ONLINE_HERO_DEF`
(`CREATURE_HERO` by default), so the first tests do not depend on dressing a puppet.

## Fable's own monsters

While the mod is connected it switches off the local creature generators
(`client-mod/src/game/spawns.cpp`), so the only monsters in the region are the server's.
On disconnect they are switched back on, so a single-player game is unaffected. Creatures
already placed in the level files are still there: removing those is the next step, along
with guards, villagers and shopkeepers.

## Positions during the first tests

The server is authoritative, and a client may only ever send intent. Fable's own input is
not mapped yet, so the mod can report where the local game put its hero
(`hero_pos`), and the server throws that away unless it is started with
`FO_TRUST_CLIENT_POS=1`. With that on, a client could put its hero anywhere in the region,
so it belongs on a test machine and nowhere else. The attempt is recorded either way.

## Trying it

- Without a server: `FABLE_ONLINE_DEV=1` and F8 in game spawns one puppet that walks a
  circle around the hero. F6 toggles the local creature generators, F7 writes the current
  state to `fable_online.log`.
- With a fake server: `node tools/dev-world/fake-world.mjs` serves frames with a hero
  walking a circle and a bandit pacing nearby, so puppets can be watched without accounts.
- With the real server: the launcher passes the address and the one-time ticket.

## What is not done yet

Animations still come from whatever Fable decides, rather than from the server's animation
field; the move actions in the notes are what will fix that. Name tags above stand-ins are
the chat thread's overlay work. Hits on a puppet are not yet routed to the server.
