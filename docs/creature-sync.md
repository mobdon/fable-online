# Driving local creatures from the server

The server decides where every monster is and what it does (`server/src/world/ai/`). Each
player's Fable still draws them, so the mod has to make the creatures in the local game
copy the server. This is the plan for that, building on the hero work in
`tools/re-notes/02-hero-live-state.md` and the script interface in
`tools/re-notes/04-script-interface.md`.

## What the server sends

Every tick, each player gets the entities in their channel (`EntityState`):

| Field | Meaning |
| --- | --- |
| `id` | server id. Creatures are 1,000,000 and up, heroes are below |
| `def` | creatures only: Fable def name, e.g. `CREATURE_BANDIT_GRUNT_LEVEL1` |
| `x, y, z` | position in the region's local frame (metres, z up), the frame `.tng` files use |
| `yaw` | facing; forward is (-sin yaw, cos yaw), the same as hero input |
| `anim` | what the creature is doing: see the table below |
| `health` | current health, for health bars and death |

Region-local coordinates are converted to Fable's world position in the mod:
`world = (MapX + x, MapY + y, z)`, with MapX/MapY from `FinalAlbion.wld` for the region.
The first step of phase A checks this formula against the live hero position at a known
`.tng` marker before anything else depends on it.

## The four jobs in the mod

### 1. Stop the local game making its own monsters

In a region the server runs, the local game must not spawn or run its own hostile
creatures, or every player sees a second, different set.

- Hook the creature generator (`CTCCreatureGenerator`, `CTCCreatureGeneratorCreator` and
  `CTCActivationReceptorCreatureGenerator` in the RTTI dump) so it does
  nothing in server-run regions. Placed creatures from the `.tng` are removed on region
  load by def (bandits, Hobbes, wolves, balverines and so on).
- Quests stay client-side. Creatures a quest script spawns are left alone: the hook only
  blocks generators and `.tng` placements, not script `CreateNear` calls. Placed creatures
  with a ScriptName are quest creatures too: the server does not spawn them and the mod keeps
  the level's copy.
- The server lists the defs it spawns in its welcome (`spawn_defs`). The mod drops a
  level-placed creature only when its def is on that list, so a server with no spawn data
  (or an older one) leaves the level as it is.
- Villagers, guards and traders are untouched for now; they stay local and simple.

### 2. Make a puppet for each server creature

When a snapshot has an id the mod hasn't seen, it creates the creature with Fable's own
spawn call (the `CreateNear` / `ObjectCreate` slots of `CGameScriptInterface`, +0x118 and
+0x120, still to map) using `def` and the converted position. The mod keeps a table
`server id -> CThing*`. An id that disappears from snapshots for 2 seconds, or a region
change, removes the puppet.

The puppet's own brain must be off, or it will fight the server. Two options, in order of
preference:

1. Put it under scripted control (`CTCScriptedControl`), which is how quests freeze a
   creature and drive it with actions. Its AI stops; it only does what it is told.
2. If that leaves bits of AI running, detach or neuter its brain (`CAIBrain` and the
   `CAIStateGroup_*` classes) after spawn.

Scripted control already has the actions needed: `CActionMoveTo`, `CActionPlayAnimation`,
`CActionPlayCombatAnimation`, `CActionFireProjectileWeapon` (archers) and
`CActionStopScripting`.

### 3. Move it smoothly

Snapshots arrive 15 to 20 times a second. The mod renders creatures about 100 ms in the
past, interpolating between the two snapshots around that time, so motion is smooth even
with jitter.

- Preferred: issue a `CActionMoveTo` (scripted-control action) toward the interpolated
  point about 200 ms ahead, at walk or run speed from `anim`. Fable then plays its own
  walk, run and turn animations, so it looks right.
- If the puppet drifts more than 1.5 m from where the server says, write the position
  straight into its physics component (the same layout as the hero:
  `thing+0x60 -> +0x0C xyz`), which snaps it.
- Facing comes from `yaw` when the creature stands still (attacking, alert, strafing).

### 4. Play what the server says it is doing

| anim | Server meaning | Local action |
| --- | --- | --- |
| 0 Idle | standing | stop moving; Fable's idle plays |
| 1 Walk | wandering | MoveTo at walk speed |
| 2 Run | chasing, fleeing, going home | MoveTo at run speed |
| 3 Attack | swinging now (archers: `CActionFireProjectileWeapon`) | `CActionPlayCombatAnimation` with the def's attack |
| 4 Alert | just noticed someone | turn to face, alert animation |
| 5 Hit | staggered | flinch animation |
| 6 Death | dead | kill the puppet locally (health 0), let the corpse and drops play |
| 7 Leap | balverine lunge | jump attack |
| 8 Strafe | circling between bites | MoveTo sideways, face the target |
| 9 Block | guard up | block animation |
| 10 Taunt | (later) | taunt animation |

Health from snapshots is written to the puppet so Fable's own health bar shows. Damage
numbers and hit effects come from the combat thread's events, not from the puppet
being hit locally: the local game must not deal damage to puppets on its own.

## Hits from the local hero

When the player swings at a puppet, the local game would normally apply damage. The mod
blocks that for server creatures and sends only the intent (attack button, target id) as
input. The server's combat module decides the hit and the next snapshot shows the result.
This keeps the server authoritative, which the economy needs (kills drop loot).

## Order of work

1. Coordinates: log the hero position next to a `.tng` marker and confirm the world =
   MapX/MapY + local formula.
2. One frozen puppet: spawn a single bandit at a fixed server position, AI off.
3. Movement: follow snapshot positions with interpolation and MoveTo.
4. Animations: map the anim table for bandits first, then Hobbes, wolves, balverines.
5. Suppress local generators in server-run regions.
6. Hits: route the local hero's attacks on puppets to the server.

Bandwidth: with 200 creatures in a region the snapshot gets large, so the next network
step (already planned for heroes) is sending only creatures within about 60 m of the
player and only fields that changed.
