@echo off
title Zero Ichi - download packages
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0offline\get-packages.ps1"
echo.
pause
