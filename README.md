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
| `launcher/` | `fable_online_launcher.exe`: login, news, region select, patching, then starts Fable with the DLL injected |
| `tools/dev-gateway/` | Dev-only stand-in for the gateway, so the launcher runs before `server/` does |
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

## The launcher

`fable_online_launcher.exe` opens a window with news, login (or create an
account), the region list and a Play button. On start it asks the gateway for
the patch manifest and brings every file next to it up to date (SHA-256
checked, paths can't leave the folder). If the launcher itself changed, it
swaps itself out and restarts. Play asks the gateway for a one-time join
ticket and starts Fable.exe with the mod injected; the mod gets its settings
from `FABLE_ONLINE_*` environment variables (`MODE`, `GATEWAY`, `REGION`,
`ACCOUNT`, `TICKET`, `HOST`, `PORT`).

The API it speaks is in `shared/protocol/launcher-api.md`. Settings live in
`%APPDATA%\FableOnline\launcher.ini`; "Remember me" stores only a refresh
token, encrypted with Windows DPAPI.

```
fable_online_launcher.exe                         window (default)
fable_online_launcher.exe --gateway https://...   use another gateway (saved)
fable_online_launcher.exe --game "D:\Games\Fable The Lost Chapters"
fable_online_launcher.exe --offline               no window, no login: just start Fable with the mod
fable_online_launcher.exe --probe                 same, with the field probe on (probe.bat)
fable_online_launcher.exe --server                server-mode Fable.exe for a region host
```

Plain `http://` is refused except for `localhost`/`127.0.0.1`. Release builds
bake in the real gateway with `-DFO_DEFAULT_GATEWAY=https://...`.

### Try it without a server

```
node tools/dev-gateway/gateway.mjs --files <folder with the files to serve as the patch>
fable_online_launcher.exe        # defaults to http://127.0.0.1:8787
```

Logic tests: configure with `-DFO_BUILD_TESTS=ON`, start the dev gateway with
`--files` pointing at a folder holding a copy of `fable_online.dll`, then run
`launcher_tests.exe http://127.0.0.1:8787` from an empty folder.
