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
