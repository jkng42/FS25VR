@echo off
rem Builds build\dinput8.dll. Needs Visual Studio 2022+ (C++ desktop workload), CMake and Ninja.
setlocal
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS set "VCVARS=D:\Program Files\Visual Studio 2026 Insider\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo Visual Studio C++ tools not found. Install the "Desktop development with C++" workload.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >nul || exit /b 1
cmake --build "%~dp0build" || exit /b 1
