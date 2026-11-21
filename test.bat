@echo off
rem ===== IMEIndicator config tests (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
rem
rem Test exe goes into bin\test\ so CfgSave/CfgLoad (which use the exe's own
rem directory) never touch the real IMEIndicator.json next to the main program.
setlocal
pushd "%~dp0"

call "build_env.bat"
if errorlevel 1 (
    popd
    exit /b 1
)

if not exist "bin\test" mkdir "bin\test"

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Fobin\test\ ^
   tests\config_test.c src\config.c src\json.c ^
   /link /OUT:bin\test\config_test.exe ^
   user32.lib shell32.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE

if errorlevel 1 (
    echo [ERROR] test build failed
    popd
    exit /b 1
)

bin\test\config_test.exe
if errorlevel 1 (
    echo [ERROR] tests failed
    popd
    exit /b 1
)

echo [OK] all config tests passed
popd
