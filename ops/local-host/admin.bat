@echo off
rem Admin jobs on the server's heroes (run while that hero is offline):
rem   admin.bat list
rem   admin.bat graduate Mob
rem   admin.bat place Mob BowerstoneSouth
setlocal
call "%~dp0find-root.bat" || (pause & exit /b 1)
cd /d "%ROOT%server"
call npm run admin -- %*
