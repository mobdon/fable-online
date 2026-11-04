@echo off
rem Lets friends reach the server through Windows Firewall (TCP 7777). Right-click, Run as administrator.
netsh advfirewall firewall add rule name="Chapters of the Lost server" dir=in action=allow protocol=TCP localport=7777
pause
