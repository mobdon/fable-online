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

`.ClearCommands` (+0x840) only zeroes component 0x30 +0x1C; it does not touch this queue.

## Creature generators

Fable spawns region monsters from generator components. In a region the server runs, these
must do nothing, or every player sees a different set of monsters.

| Class | Vtable | Notes |
| --- | --- | --- |
| `CTCCreatureGenerator` | `0x01268C24` | slot +0x24 is its work slot (372 instructions), no arguments |
| `CTCCreatureGeneratorCreator` | `0x0126EB3C` | the same slot, same shape |
| `CTCActivationReceptorCreatureGenerator` | `0x012413EC` | generators a trigger switches on; not patched yet |

`client-mod/src/game/spawns.cpp` points both +0x24 slots at a bare `ret` while suppression
is on, and puts the originals back afterwards. Creatures already placed in the level's
`.tng` are left alone for now: they are the next thing to handle.
