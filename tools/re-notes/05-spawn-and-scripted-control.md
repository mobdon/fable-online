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

The mod does not use these yet: it writes the position and facing into the physics
component instead (`02-hero-live-state.md`), which needs no mapping of the move modes and
cannot leave a creature walking somewhere the server never asked for. The move actions are
the next step, for walk and run animations that match the speed the server reports.

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
