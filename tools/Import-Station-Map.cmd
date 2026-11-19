@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Import-Station-Map.ps1" %*
set "result=%errorlevel%"
if not "%result%"=="0" echo Station map import failed.
pause
exit /b %result%
