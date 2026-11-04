# Spawning and driving creatures

Found by reading Fable.exe's script runner (the long string-compare chain at about
0x00CBE000..0x00CD2000), which is where text commands such as `CreateNear` and `.RunTo`
turn into calls on the engine. The mod makes the same calls, so it stays on paths the
retail game already uses every time a quest runs. All addresses are fixed: Fable.exe has
no ASLR. See also `04-script-interface.md`.

## References to things

The script interface hands out a 12-byte reference ("intelligent pointer"), laid out as
`{ vtable, CThing*, refcount block }`. Everything below takes one of those, not a raw
pointer.

| What | Address | Shape |
| --- | --- | --- |
| copy constructor | `0x004ABE90` | `__thiscall (dest, const ref* src)` |
| destructor | `0x004AA840` | `__thiscall (ref*)` |
| is the thing still alive | `0x004AB130` | `__thiscall (ref*) -> bool` |
| `CCharString` ctor / dtor | `0x0099EBF0` / `0x0099EAE0` | as in `04-script-interface.md` |

## Script interface slots

`CGameScriptInterface` at `[0x0143E8F8]`, vtable `0x01260F0C`. The argument count comes
from each handler's `ret n`.

| Slot | Script command | Signature |
| --- | --- | --- |
| +0x118 | the hero | `() -> ref*` (a reference the caller copies) |
| +0x120 | a thing by script name | `(ref* out, CCharString* name) -> out` |
| +0x16C | `Create` | `(out, def, nearValue, scriptName, flags)` |
| +0x170 | `CreateNear` | `(out, def, nearValue, float radius, scriptName, flags)` |
| +0x764 | `TeleportThing`, `.ResetPos` | `(ref* thing, ref* target, float, bool, int)` — to another thing, not to coordinates |
| +0x768 | `.LookInDirection` | `(ref*, float radians, bool)` |
| +0x7BC | `.SetFree` | `(ref*)` |
| +0x7FC | `.Drawable` | `(ref*, bool)` |
| +0x728 | `.SetAttackable` | `(ref*, bool)` |
| +0x810 | `.SetDamageable` | `(ref*, bool)` |
| +0x814 | `.Killable` | `(ref*, bool, bool)` |
| +0x840 | `.ClearCommands` | `(ref*)` |
| +0x424 | `.ModifyHealth` | `(ref*, float delta, bool)` |
| +0x7A0..+0x7AC | `.EntitySetMax{Walking,Running}Speed` | `(ref*, float)` |
| +0x800 | `PauseThing`, and the "stop scripting" step | `(ref*, int mode)` |
| +0xBB4 / +0xBB8 | `.AddScriptedMode` / `.RemoveScriptedMode` | `(ref*, int)` / `(ref*)` |

### The CreateNear sequence

The runner does this, and so does `client-mod/src/game/script.cpp`:

1. get the hero's reference (+0x118) and copy it,
2. call the near thing's own `vtable+0x18`, which returns the value the create call wants
   and leaves its arguments on the stack (it pops nothing),
3. call +0x170 with `(out, def, that value, radius, scriptName, 0)` — six arguments, which
   is what the handler at `0x0089F300` pops (`ret 0x18`).

The engine picks the exact spot within `radius` of the hero; the mod then writes the
position it wants straight into the physics component.

## Driving a creature

A thing a script holds is wrapped in a handle whose vtable is at `0x0128D86C`
(`CScriptGameResourceObjectScriptedThing`). Its slots are where `.RunTo` and friends end
up, and they build the `NTCScriptedControl::CAction*` objects:

| Slot | Used by | Action built |
| --- | --- | --- |
| +0x10 | `.WalkUpToThing` | `CActionMoveTo` (`0x009051F0`) |
| +0x14 | `.WalkTo`, `.RunTo`, `.SneakTo` | `CActionMoveTo`, 7 arguments: `(target, float, mode, 0, bool, bool, bool)` |
| +0x18 | `.FollowNavRoute` | `CActionMoveTo` along a route |
| +0x1C | `.FollowThing` | `CActionFollowCreature` |
| +0x48 | `.PlayAnimation` | `CActionPlayAnimation` (8 arguments) |
| +0x4C | `.PlayCombatAnim` | `CActionPlayCombatAnimation` |
| +0x5C | `.WalkUpToThing` | `CActionMoveTo` with a stopping distance |
| +0x64 | `.LookAt` | `CActionTurnToFacePosition` |
| +0x28 / +0x58 | `.StopFollowingThing`, `.ClearCommands` | drops queued actions |

The mod does not use the move actions: it writes the position and facing into the physics
component (`02-hero-live-state.md`), which cannot leave a creature walking somewhere the
server never asked for, and plays the walk, run or idle loop itself (below).

### What the move slot takes

Slot +0x14 (`0x00903E20`, `ret 0x1C`) is `(ref* target, float stopDistance, int mode, int,
bool, bool, bool)`. The target is another 12-byte thing reference: its vtable +0x12C is
"is valid" and +0x2C returns the `CThing*`. `.RunTo` passes `(target, 0.3, 1, 0, 1, 0, 1)`
by default. The mode maps to a move type: 0 -> 3, 1 -> 5, anything else -> 1. There is no
form that takes coordinates: `CActionMoveTo` (0x50 bytes, ctor `0x009051F0`) keeps a
reference to the target thing, so driving a puppet with it needs a second thing to aim at.

## Playing animations (used by the puppets)

**Correction (in-game test):** a `CREATURE_HERO` thing has no `CTCScriptedControl`. Components
come from the def through a registry by name (`0x004EFFB4` registers the factory `0x004D311E`
as "CTCScriptedControl"), and the hero's def does not list it. Bit `id` of the dwords at
`thing+0x20` says which components a thing has (bit 31 of +0x20 is 0x1F). So the puppets skip
the scripted queue and do what `CActionPlayAnimation::Update` (`0x00712900`) does, on the
creature itself:

| What | Address | Shape |
| --- | --- | --- |
| refuse new actions now | `0x0085F0C0` | `CThing::() -> bool` |
| animation by name | `0x00662D80` | `CThing::(CCharString*, int -1) -> anim`, `ret 8` |
| creature play-animation action | `0x008425E0` | ctor, 0xB4 bytes: `(creature, anim, b, looping, loops, out[2], b, global, b, 0, seq)` |
| set as the current action | `0x006644F0` | `CThing::(action*) -> bool`, keeps a clone (action vtable +0x2C) |
| queue behind the current one | `0x00664490` | `CThing::(action*)`, keeps a clone |
| current action | `thing+0x100` | its byte +0x61 set = finished |

The caller deletes its own action afterwards (vtable slot 0 with 1), as the engine does.

The scripted-control path below is still right for creatures whose def has the component.

The handle is not needed: every handle slot finds the thing's `CTCScriptedControl`
(component id 0x1F, vtable `0x012658E4`) and queues an action on it. The mod does the same.

| What | Address | Shape |
| --- | --- | --- |
| component lookup | inline | thing+0x20 sign bit set, then sorted `(id, component*)` pairs from `[thing+0x44]` to `[thing+0x48]` |
| `operator new` | `0x00BFEA1A` | `__cdecl (size)` |
| `CActionPlayAnimation` ctor | `0x009034F0` | `__thiscall`, 11 args, object 0x28 bytes, vtable `0x0128D6F4` |
| queue an action | `0x007137D0` | `CTCScriptedControl::(action*)`, takes ownership |
| stop and drop the queue | `0x00713630` | `CTCScriptedControl::()` |
| end the creature's own action | `0x00662650` | `CThing::()`, only when `thing+0x6C` bit 3 is set |
| queue update | `0x00713910` | runs whenever the queue is not empty (called from `0x006AD9B8`) |

`.PlayLoopingAnim` (handle +0x50) builds `ctor(name, 0, 1, loops, 0, seq, 0, 1, [0x01375748], 0, 0)`;
`.PlayAnimation` (+0x48) builds `ctor(name, b, 0, -1, 0, seq, ...)`. When the action runs,
`0x00662D80` looks the name up in the creature's animation map (`[thing+0x70]` -> `+0x34`),
the same map the movement code reads `ST_WALK` and `ST_RUN` from (`0x00663610`). So
`ST_IDLE`, `ST_WALK`, `ST_RUN` and the other `ST_*` names play each creature's own loop.

The puppets map the server's anim ids to these names (`client-mod/src/game/puppet.cpp`,
`spec_for`): `ST_IDLE`, `ST_WALK`, `ST_RUN`, `ST_STRIKE_LEFT`/`ST_STRIKE_RIGHT`,
`ST_IDLE_MELEE`, `ST_BACK_HIT`, `ST_DIE` then `ST_DEATH_FRAME`, `ST_LEAP_IN`,
`ST_BLOCK_LOOP`, `ST_TAUNT`. All appear in `names.bin`. Which creature defs map each one is not
checked yet: a name a creature lacks falls back to a default inside `0x00662D80`.

`.ClearCommands` (+0x840) only zeroes component 0x30 +0x1C; it does not touch this queue.

## Creature generators

Fable spawns region monsters from generator components. In a region the server runs, these
must do nothing, or every player sees a different set of monsters.

| Class | Vtable | Notes |
| --- | --- | --- |
| `CTCCreatureGenerator` | `0x01268C24` | slot +0x24 is its work slot (372 instructions), no arguments |
| `CTCCreatureGeneratorCreator` | `0x0126EB3C` | the same slot, same shape |
| `CTCActivationReceptorCreatureGenerator` | `0x012413EC` | generators a trigger switches on; needs no patch (below) |

`client-mod/src/game/spawns.cpp` points both +0x24 slots at a bare `ret` while suppression
is on, and puts the originals back afterwards.

The activation receptor only switches a generator on: its slot +0x70 (`0x007F7650`) looks up
the owner's generator component (id 0x3D in the thing's component map at +0x44) and sets its
active byte (+0x4F). The generator's work slot is already a `ret`, so nothing spawns.

## Creatures placed in the level files

The `.tng` `AICreature` entries do not come from a generator: the level loader builds each
one. The loader registers its thing kinds at `0x00522A20` (`"AICreature"` -> factory
`0x005272E0`, which allocates 0x1D8 bytes and runs the `CThingAICreature` constructor,
vtable `0x0127293C`).

| What | Address | Shape |
| --- | --- | --- |
| load one thing ("Load Single Thing") | `0x0051FD80` | `__thiscall (CThingManager*, parser*, int mode) -> CThing*`, `ret 8`; 0 when the entry is dropped |
| its callers | `0x0052062E`, `0x00520BA1`, `0x00520F9A`, `0x00521155`, `0x00521521` | the three level loaders and two save loaders; all handle a 0 result |
| read an entry's header | `0x004C81F0` | ecx = parser, edx = header; header +0 UID (64-bit), +8 `DefinitionType` (CCharString), +0xC `Player` |
| its callers | `0x004C89A8`, `0x004C9252`, `0x0051FE07` | the first two run inside the load above |
| `CThing::Remove(bool)` | `0x004C9B80` | `__thiscall (thing, bool)`, `ret 4`; the loader calls it with 1 on a thing it cannot keep, then returns 0 |
| thing ScriptName | thing +0x74 | CCharString, set by the text parser at `0x004CAA00` |

A CCharString is a pointer to a block whose first field is the `char*`.

`spawns.cpp` points the five load calls at the mod and the three header reads at a small
thunk that records the entry's def. When the thing that comes back is a `CThingAICreature`
with no ScriptName and a def from the server's `spawn_defs` list, the mod removes it with
`0x004C9B80(1)` and returns 0, which is the loader's own path for a dropped thing.
Script-created creatures (`CreateNear` and friends) never pass through this loader.

Not checked in game yet: whether Fable records a removed level creature as dead in the save,
which would keep it gone in an offline game played later from the same save.
