# Co-op spirits (CTCCoopSpirit) and the multi-player slots

Read statically from the Steam Fable.exe (16,666,624 bytes). Nothing here has been run in game yet.

## What it is

A cut **same-screen co-op mode**. Players two to four each held a pad and flew a small
floating spirit around the hero. The spirits attacked enemies, competed for points, and
the first spirit to 20 points swapped bodies with the hero. It is not a network feature:
there is no packet, socket or remote-input path anywhere in it.

## Classes

| Class | vtable | Role |
| --- | --- | --- |
| `CTCCoopSpirit` | 0x0125B264 (28 slots), component id **0xC7**, object size 0x34 | The spirit's brain |
| `CCoopSpiritDef` | 0x01232214 | Sub-def the component reads on init. **No def in names.bin has one** |
| `CInputProcessControlSpirit` | 0x0125D110 | Pad input for a spirit player (built in the `CPlayer` set-up at 0x4896B4) |
| `CTCTargetingSpirit` | 0x012419A4 | Spirit targeting |
| `CCreatureAction_SpiritAttack` / `_SpiritHitResponse` / `_SpiritReturnToHero` | 0x01275D74 / 0x01275B34 / 0x01275C54 | Creature actions |
| `NPlayerGui::CDrawSpiritScores` / `CDrawSpiritsHealth` / `CDrawSpiritTargetingCursor` | 0x0125A598 / 0x0125A538 / 0x0125A5E8 | HUD pieces |

Factory: 0x004D55D0 (`new(0x34)` then ctor 0x00670050), registered by name at 0x004F6527.

## CTCCoopSpirit fields

| Offset | Meaning |
| --- | --- |
| +0x04 | Owning thing |
| +0x0C | Frames since last input (drives the leash) |
| +0x10 | Score. At 20 or more the spirit takes over the hero |
| +0x18/+0x1C/+0x20 | Input stick vector, consumed and cleared each frame |
| +0x24 | `CPlayer*` the spirit follows |
| +0x28 | `CCoopSpiritDef*` (null means the update crashes) |
| +0x2C | Thing pointer (vtable 0x01238C6C), the bound target |

## Update (slot +0x24, 0x00670A10)

1. Rebinds its target through the global at 0x013CA828 using def+0x30 (0x006705F0).
2. 0x00670820: clamps speed, writes physics velocity (thing+0x60 +0x28..+0x30) and turns the thing to face its velocity.
   - 0x006702E0: turns the stick vector into a camera-relative force.
   - 0x006701A0: leash. After a while with no input it is pulled back toward its player's hero.
   - 0x0066FC90: keeps the spirit inside the camera's view (shared screen).
3. 0x0066FB80: pushes away from the other three player things when too close.
4. 0x00670470: hover spring that holds it a fixed height above the ground (0x00666D40).
5. If thing+0xB4 (life) drops to zero or below, the thing is removed (0x004C9B80, CThing remove).
6. 0x00670710: if score >= 20, every spirit's score resets and 0x0066FF20 swaps: this spirit's player becomes the main player, and the old main player is re-made as a spirit at the same spot.

## The player slots (worth knowing on its own)

- Player manager: `[[0x013B8A1C] + 0x30]`. +0x0C/+0x10 vector of `CPlayer*`, +0x1C main player index. Loops cap at 4.
- `CPlayer`: +0x28 player index, +0x2C controlled-thing pointer (read with 0x00487DC0).
- 0x00449880 checks a slot. 0x004498C0 gets `CPlayer*` by index. 0x00449CB0 makes slot N the main player.
- 0x00449730 returns the def name `COOP_SPIRIT_PLAYER_ONE..FOUR` for a slot.
- 0x00449B20(index, defName) calls `CPlayer::InitCharacterAs` (0x0048A070), which builds that player's character from a def.

## Fit for showing remote players

Poor. It takes a stick vector, not positions. It has no animation input. It forces a hover
height, pulls toward the hero and stays on the camera's screen, so a walking hero model
on it would float and be dragged into view. It also needs a `CCoopSpiritDef` that does not
exist in the retail defs. The physics-driven puppets in `client-mod/src/game/puppet.cpp`
remain the right path for other players.

What carries over is the presentation: the `MULTIPLAYER_<colour>` glows, the
`MULTIPLAYER_SPIRIT_HIT_*` / `_GET_HIT_*` effects, `SPRITE_MULTIPLAYER_ORB`, and the
spirit HUD layout, applied to party members.
