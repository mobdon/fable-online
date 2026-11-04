@echo off
rem One-time setup: installs the server and reads regions, exits, doors, monsters and shops
rem from YOUR Fable install. Nothing from the game leaves this PC. See README.txt.
setlocal
call "%~dp0find-root.bat" || goto :fail
where node >nul 2>&1 || (
  echo Node.js is not installed. Get the LTS version from https://nodejs.org and run this again.
  goto :fail
)
set "FABLE=%~1"
if "%FABLE%"=="" set "FABLE=C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
if not exist "%FABLE%\Fable.exe" (
  echo Fable is not in "%FABLE%".
  echo Run it again with your Fable folder, for example: 1-setup-server.bat "D:\Games\Fable The Lost Chapters"
  goto :fail
)
cd /d "%ROOT%server"
echo Installing the server...
call npm install --no-audit --no-fund || goto :fail
echo Reading game data from "%FABLE%"...
call npm run extract -- "%FABLE%" || goto :fail
call npm run extract-town -- "%FABLE%" || echo   (town data failed; the server uses a test village)
node ..\tools\extractor\regions.mjs "%FABLE%" || goto :fail
node ..\tools\extractor\spawns.mjs "%FABLE%" || echo   (spawns failed; no monsters)
node ..\tools\extractor\things.mjs "%FABLE%" || echo   (things failed; no doors or region exits)
node ..\tools\extractor\quests.mjs "%FABLE%" || echo   (quests failed)
node ..\tools\extractor\navmesh.mjs "%FABLE%" || echo   (navmesh failed; heroes and monsters move without walls)
echo.
echo Done. Next: 2-start-server.bat
pause
exit /b 0
:fail
pause
exit /b 1
