# Fable.exe overview (Steam, Fable: The Lost Chapters)

Checked 2026-11-03 against the Steam install (Fable.exe, 16,666,624 bytes).

| Fact | Value | Why it matters |
| --- | --- | --- |
| Format | PE32, Intel 80386, GUI | Mod DLL and launcher must be 32-bit |
| Build timestamp | 2005-07-15 15:53:03 UTC | Use this to tell exe versions apart |
| Image base | 0x00400000 | Where code lives in memory |
| DLL characteristics | 0x0000 (no ASLR, no DEP flag) | **Addresses are the same every run**: an address found once stays valid |
| Steam DRM | none found (no steam_api, no SteamStub `.bind` section) | The exe can be started directly, no Steam wrapper to get past |
| Imports of note | d3d9.dll, DirectInput, WSOCK32 | D3D9 = where a render-off switch for server mode goes; DirectInput = input hooks |
| RTTI | 2,408 C++ class names left in the exe | Classes can be found by name in Ghidra, a huge head start |

## Classes worth starting from

- **World and regions:** `CWorld`, `CWorldMap`, `CLevelLoader`, `CRegionDef`, `CTCRegionFollower`, `CTCDRegionEntrance`, `CTCDRegionExit`
- **Creatures and AI:** 527 `Creature` classes, e.g. `CCreatureActionHeroJump`, `CAIStateGroup_MeleeCombatNearby`, `CAIStateGroup_FollowPlayer`
- **Combat:** 148 `Combat` classes, e.g. `CCombatAction_StabThing`, `CCombatAction_KickThingOnGround`
- **Input:** `CInputManager`, `CInputManagerDX`, `CAInputProcess` and ~90 `CInputProcess*` screens
- **Camera:** `CEngineCamera`, `CameraModes` family (`CFirstPerson@NCameraModes`, ...)
- **Scripting:** `CTCScriptedControl` actions (`CActionMoveTo`, `CActionPlayAnimation`, ...): the game's own way to drive a creature from script, a likely route for moving puppet heroes
- **Networking (leftover from Lionhead):** `CNetworkClient`, `CNetworkServer`, `LSocketConnError@LHNetworkLib`. Unused in the retail game as far as we know. Worth a look before writing our own transport.

## Plain-text game files we can read directly

- `user.ini` (next to Fable.exe) runs script commands at start-up: `RunScript(...)`, `ActivateQuest("Gameflow")`, `MaxThingDrawDist 128`. A cheap way to change engine settings for server mode without patching.
- `data/Levels/FinalAlbion.wld`: 398 maps, each with `MapUID`, `LevelScriptName`, `MapX`/`MapY`, `IsSea`, `LoadedOnPlayerProximity`. Region list for the server (`tools/extractor/regions.mjs`).
- `data/Misc/FinalAlbion_StartingRegionGraph.txt`: 92 regions and their neighbours. Region handoff map.
- `data/Levels/GlobalQuests.qst`: quests always active (death watch, expressions, teleport to guild, chests).
- `data/Defs/*.h` and `data/Defs/RetailHeaders/*.h`: shipped header files with enums (misc def types, text IDs, mesh IDs).

## Next to find (phase 2)

1. The player's hero object (start from `CWorld` and the player GUI classes).
2. Hero position, facing, health, will.
3. A function that creates a creature from a def name.
4. The damage / hit function under the `CCombatAction_*` family.
5. Region load / unload in `CLevelLoader`.
