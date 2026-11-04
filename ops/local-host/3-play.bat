@echo off
rem Opens the launcher against a Chapters of the Lost server.
rem   3-play.bat                  the server on this PC
rem   3-play.bat 100.101.102.103  a friend's server (their Tailscale or LAN address)
rem Log in (Register makes an account; your hero is named after it), then Enter Albion.
cd /d "%~dp0"
set "HOST=%~1"
if "%HOST%"=="" set "HOST=127.0.0.1"
taskkill /F /T /IM Fable.exe >nul 2>&1
set FABLE_ONLINE_WIDESCREEN=1
set FABLE_ONLINE_DEV=1
set FABLE_ONLINE_SUPPRESS_SPAWNS=1
fable_online_launcher.exe --gateway http://%HOST%:7777
