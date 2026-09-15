@echo off
rem ===== IMEStatus: shared MSVC x64 toolchain setup =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII breaks parsing.
rem vcvars64.bat is avoided on purpose; paths are set explicitly instead,
rem discovered from the VS install (works for any version).
set "PFX86=C:\Program Files (x86)"
if not exist "%PFX86%" set "PFX86=%ProgramFiles%"

set "VS="
set "VSW=%PFX86%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined IME_VSVER (
    if exist "%VSW%" (
        for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -version "%IME_VSVER%" -property installationPath`) do set "VS=%%i"
    )
)
if not defined VS if exist "%VSW%" (
    for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -property installationPath`) do set "VS=%%i"
)
if not defined VS if exist "C:\VS2022\VC\Auxiliary\Build\vcvars64.bat" set "VS=C:\VS2022"
if not defined VS goto no_vs

set "MSVCDIR="
pushd "%VS%\VC\Tools\MSVC"
if errorlevel 1 goto no_msvc
rem 版本号最大的目录：dir /o-n 倒序，取第一条（装了多个工具集时别挑到最后那个）
for /f "delims=" %%d in ('dir /b /ad /o-n') do if not defined MSVCDIR set "MSVCDIR=%%d"
popd
if not defined MSVCDIR goto no_msvc
set "MSVC=%VS%\VC\Tools\MSVC\%MSVCDIR%"

set "SDK=%PFX86%\Windows Kits\10"
set "SDKVER="
pushd "%SDK%\Include"
if errorlevel 1 goto no_sdk
for /f "delims=" %%d in ('dir /b /ad /o-n') do if not defined SDKVER set "SDKVER=%%d"
popd
if not defined SDKVER goto no_sdk

set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

echo [ENV] VS=%VS%
echo [ENV] MSVC=%MSVCDIR%
echo [ENV] SDK=%SDKVER%
exit /b 0

:no_vs
echo [ERROR] Visual Studio not found
exit /b 1

:no_msvc
echo [ERROR] MSVC toolset not found under %VS%
exit /b 1

:no_sdk
echo [ERROR] Windows SDK not found under %SDK%
exit /b 1