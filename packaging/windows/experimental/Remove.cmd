@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Experimental-Setup.ps1" -Operation Remove
pause
