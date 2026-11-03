# Fable Online

A multiplayer (MMO) mod for **Fable: The Lost Chapters (PC, Steam app 204030)**.
Every player needs their own copy of the game. This repo never contains any of
Lionhead's code or data: only our mod, launcher, server and tools.

Plan: https://claude.ai/artifact/H5LgN8n6CD1f8CFtjkGp1e

## Layout

| Path | What |
| --- | --- |
| `shared/protocol/` | Network messages, shared by mod and server |
| `shared/defs/` | ID tables generated from your own `game.bin` (not committed) |
| `client-mod/` | `fable_online.dll`, injected into Fable.exe (32-bit) |
| `launcher/` | `fable_online_launcher.exe`: finds the game, injects the DLL |
| `server/` | Gateway, region orchestrator, persistence (TypeScript) |
| `server/chat/` | Chat and name tag service (TypeScript, Node 22) |
| `tools/re-notes/` | What we've found inside Fable.exe, one file per system |
| `tools/extractor/` | Reads game data from a local install into `shared/defs/` |

## Phase 1: get the mod loading

Fable.exe is a **32-bit** program, so the DLL and launcher are built as 32-bit.

### Build on Windows (Visual Studio 2022 + CMake)

```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

### Build on Linux / WSL (MinGW cross-compile)

```
sudo apt install g++-mingw-w64-i686 cmake
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
cmake --build build
```

### Run

1. Put `fable_online.dll` next to `fable_online_launcher.exe`.
2. Run `fable_online_launcher.exe`. It finds the game under Steam's default
   folder, or pass the folder: `fable_online_launcher.exe "D:\Games\Fable The Lost Chapters"`.
3. Steam must be running.
4. **Phase 1 passes when** `fable_online.log` appears next to `Fable.exe` with
   a `loaded into Fable.exe` line, then a heartbeat line every few seconds while you play.

## Chat and name tags

The mod draws a chat box (bottom left) and name tags over other heroes (name, guild,
title, renown; red name = flagged for PvP). Protocol and rules: `shared/protocol/chat.md`.

### Run a chat server

```
cd server/chat
npm install            # dev tools only; the server itself has no dependencies
npm start              # dev mode: log in by name, port 7781
npm test               # 13 tests
npm run bot -- --name Maze --renown 12500 --title "Arrow Master"   # a fake player to chat with
```

Production sets `CHAT_TOKEN_SECRET` (shared with the gateway) so only launcher logins work.

### In game

Copy `client-mod/fable_online.ini.example` to `fable_online.ini` next to the DLL and set
`server=` and `name=`. Enter opens the chat box, Enter sends, Esc closes, `/help` lists
commands (`/s`, `/re` region, `/1` world, `/2` trade, `/p`, `/g`, `/w Name`, `/r`, `/ignore`, `/tags`).
While the box is open the game doesn't see your keys. `fable_online.log` reports what got
hooked and whether a camera matrix was found for the name tags.

### Tests without the game

- `client-mod/tests/run.sh`: chat box and name tag logic (any OS with a C++17 compiler).
- `tools/overlay-test/`: a stand-in for Fable.exe that loads the DLL, fakes a hero and draws a
  scene, so the overlay can be checked on Windows or under Wine
  (`cmake -DFO_BUILD_OVERLAY_TEST=ON`).
