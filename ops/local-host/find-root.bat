@echo off
rem Sets ROOT to the folder holding server\ (this folder in a test build, the repo root in a checkout).
set "ROOT=%~dp0"
if not exist "%ROOT%server\package.json" set "ROOT=%~dp0..\..\"
if not exist "%ROOT%server\package.json" (
  echo Cannot find the server folder next to this file.
  exit /b 1
)
exit /b 0
