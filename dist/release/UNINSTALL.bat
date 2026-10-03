@echo off
title fs25vr - remove VR from Farming Simulator 25
echo.
echo  fs25vr - remove VR from Farming Simulator 25
echo  ------------------------------------------------------------
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
echo.
if errorlevel 1 (
    echo  Something went wrong - see the message above.
) else (
    echo  fs25vr has been removed.
)
echo.
pause
