@echo off
title fs25vr - set the best render size for your headset
echo.
echo  fs25vr - set the best render size for your headset
echo  ------------------------------------------------------------
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -AutoResolution
echo.
if errorlevel 1 (
    echo  Something went wrong - see the message above.
) else (
    echo  Done! The game will now render at the ideal size for your headset.
)
echo.
pause
