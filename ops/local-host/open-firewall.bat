@echo off
rem Lets friends reach the server through Windows Firewall (TCP 7777 game, 7781 Realm chat). Right-click, Run as administrator.
netsh advfirewall firewall add rule name="Chapters of the Lost server" dir=in action=allow protocol=TCP localport=7777
netsh advfirewall firewall add rule name="Chapters of the Lost chat" dir=in action=allow protocol=TCP localport=7781
pause
