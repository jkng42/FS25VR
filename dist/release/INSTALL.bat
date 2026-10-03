@echo off
title fs25vr - install VR for Farming Simulator 25
echo.
echo  fs25vr - install VR for Farming Simulator 25
echo  ------------------------------------------------------------
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" 
echo.
if errorlevel 1 (
    echo  Something went wrong - see the message above.
) else (
    echo  Done! Start the game with your headset on and enable the VR mod when loading a savegame.
)
echo.
pause
