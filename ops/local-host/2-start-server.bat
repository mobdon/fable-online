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
rem The server checks every position the mod reports (speed, climbs, falls) and corrects cheats.
set FO_CLIENT_MOVE=checked
rem Region map with level sizes, so the server knows which region your Fable has loaded.
rem Older setups lack the sizes; this re-reads them from your Fable folder (a second or two).
findstr /c:"\"w\":" "..\shared\defs\regions.json" >nul 2>&1 || (
  if exist "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters\Fable.exe" node ..\tools\extractor\regions.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
)
echo Players connect to %FO_PUBLIC_HOST% port 7777. Press Ctrl+C to stop (heroes are saved).
call npm run dev
pause
