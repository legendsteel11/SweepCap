@echo off
rem SweepCap 빌드. 인자로 프리셋 이름을 준다 (기본 debug).
rem   build.bat            -> debug
rem   build.bat release    -> release
setlocal

set PRESET=%1
if "%PRESET%"=="" set PRESET=debug

set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=C:\Program Files (x86)"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" goto :no_vs

rem 경로에 공백이 있어서 for /f 안에서 따옴표를 쓰면 cmd가 명령을 쪼갠다.
rem 설치 폴더로 옮겨 가서 따옴표 없이 부른다.
set "VSPATH="
pushd "%PF86%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
popd
if not defined VSPATH goto :no_cpp

rem vcvarsall 내부가 bare `vswhere.exe`를 부르는데, 이 PC처럼
rem NoDefaultCurrentDirectoryInExePath가 켜져 있으면 stderr로 오류를 뱉는다.
rem 동작에는 지장이 없고 우리 오류처럼 보이므로 눌러 둔다.
rem 진짜 실패는 바로 아래 where cl.exe 가 잡는다.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul
if errorlevel 1 goto :no_vcvars

where cl.exe >nul 2>&1
if errorlevel 1 goto :no_cl

cd /d "%~dp0"

if not exist "res\sweepcap.ico" (
    echo 아이콘 생성 중...
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 res\sweepcap.ico
    if errorlevel 1 exit /b 1
)

cmake --preset %PRESET%
if errorlevel 1 exit /b 1

cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1

echo.
echo 빌드 완료: build\%PRESET%\SweepCap.exe
exit /b 0

:no_vs
echo [오류] vswhere를 찾을 수 없다. Visual Studio가 설치돼 있는지 확인한다.
exit /b 1

:no_cpp
echo [오류] C++ 도구 집합이 설치된 Visual Studio를 찾을 수 없다.
echo        "C++를 사용한 데스크톱 개발" 워크로드를 설치한다.
exit /b 1

:no_vcvars
echo [오류] vcvarsall 실행 실패
exit /b 1

:no_cl
echo [오류] vcvarsall을 불렀는데도 cl.exe가 PATH에 없다.
exit /b 1