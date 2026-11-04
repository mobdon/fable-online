@echo off
rem Runs the Chapters of the Lost server on this PC. Leave this window open while you play.
rem   2-start-server.bat                 just you (and walkers) on this PC
rem   2-start-server.bat 100.101.102.103 friends join at that address (your Tailscale or LAN IP)
setlocal
call "%~dp0find-root.bat" || (pause & exit /b 1)
cd /d "%ROOT%server"
if not exist node_modules (
  echo Run 1-setup-server.bat first.
  pause
  exit /b 1
)
set "FO_PUBLIC_HOST=%~1"
if "%FO_PUBLIC_HOST%"=="" set "FO_PUBLIC_HOST=127.0.0.1"
rem Test realm: new heroes start in the shared world (at the Guild), not in a private childhood.
set FO_NEW_HERO_PHASE=world
rem The launcher has no hero screen yet: an account's first join makes a hero named after it.
set FO_AUTO_HERO=1
rem The server moves every hero itself from the stick the mod sends; a position report only
rem places a hero where their Fable loaded its save, once after joining. The mod follows the server.
set FO_CLIENT_MOVE=anchor
rem Walkable ground per region (shared\defs\nav from 1-setup-server.bat): walls for heroes and paths for monsters.
set FO_NAV_DIR=..\shared\defs\nav
rem Region map with level sizes, so the server knows which region your Fable has loaded.
rem Re-read from your Fable folder on every start (a second or two), so older setups get it.
set "FABLE=C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
if exist "%FABLE%\Fable.exe" node ..\tools\extractor\regions.mjs "%FABLE%"
rem Test realm: every hero can fight every other hero (no Guild mission needed).
set FO_PVP_ALL=1
echo Players connect to %FO_PUBLIC_HOST% port 7777 (Realm chat on 7781). Press Ctrl+C to stop (heroes are saved).
call npm run dev
pause
