@echo off
rem Build SweepCap. Pass a preset name; defaults to debug.
rem   build.bat            -> debug
rem   build.bat release    -> release
setlocal

set PRESET=%1
if "%PRESET%"=="" set PRESET=debug

set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=C:\Program Files (x86)"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" goto :no_vs

rem Quoting the path inside for /f makes cmd split the command, so change into
rem the installer directory and call it without quotes. The leading .\ is
rem required on machines where NoDefaultCurrentDirectoryInExePath is set.
set "VSPATH="
pushd "%PF86%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
popd
if not defined VSPATH goto :no_cpp

rem vcvarsall itself calls a bare `vswhere.exe`, which fails on machines with
rem NoDefaultCurrentDirectoryInExePath set and writes an error to stderr. It is
rem harmless but looks like ours, so it is suppressed. The `where cl.exe` check
rem below catches an actual failure.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul
if errorlevel 1 goto :no_vcvars

where cl.exe >nul 2>&1
if errorlevel 1 goto :no_cl

cd /d "%~dp0"

if not exist "res\sweepcap.ico" (
    echo Generating the icon...
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 res\sweepcap.ico
    if errorlevel 1 exit /b 1
)

cmake --preset %PRESET%
if errorlevel 1 exit /b 1

cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1

echo.
echo Built: build\%PRESET%\SweepCap.exe
exit /b 0

:no_vs
echo [error] vswhere not found. Check that Visual Studio is installed.
exit /b 1

:no_cpp
echo [error] No Visual Studio installation with the C++ toolset was found.
echo         Install the "Desktop development with C++" workload.
exit /b 1

:no_vcvars
echo [error] vcvarsall failed.
exit /b 1

:no_cl
echo [error] cl.exe is not on PATH even after vcvarsall.
exit /b 1