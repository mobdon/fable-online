@echo off
rem Bot players that walk around your hero, so you can see stand-ins driven by the server
rem on one PC. Needs the server (2-start-server.bat) running on this PC.
rem   4-start-walkers.bat           3 walkers around whoever played last
rem   4-start-walkers.bat Mob       3 walkers around the hero called Mob
rem   4-start-walkers.bat Mob 6     6 walkers
setlocal
call "%~dp0find-root.bat" || (pause & exit /b 1)
set "COUNT=%~2"
if "%COUNT%"=="" set COUNT=3
cd /d "%ROOT%server"
if "%~1"=="" (
  call npm run walkers -- --count %COUNT%
) else (
  call npm run walkers -- --near %~1 --count %COUNT%
)
pause
