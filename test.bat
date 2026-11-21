@echo off
rem ===== IMEIndicator tests (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
rem
rem Config tests run inside bin\test\ so CfgSave/CfgLoad (which use the exe's
rem own directory) never touch the real IMEIndicator.json next to the main program.
setlocal
pushd "%~dp0"

call "build_env.bat"
if errorlevel 1 (
    popd
    exit /b 1
)

if not exist "bin\test" mkdir "bin\test"

rem ---- config layer: JSON round-trip, defaults, inherit, migration, blacklist ----
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Fobin\test\ ^
   tests\config_test.c src\config.c src\json.c ^
   /link /OUT:bin\test\config_test.exe ^
   user32.lib shell32.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] config_test build failed
    popd
    exit /b 1
)

bin\test\config_test.exe
if errorlevel 1 (
    echo [ERROR] config tests failed
    popd
    exit /b 1
)

rem ---- shape rasterizer: area, antialiasing, premultiplied alpha ----
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Fobin\test\ ^
   tests\shape_test.c src\shape.c ^
   /link /OUT:bin\test\shape_test.exe ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] shape_test build failed
    popd
    exit /b 1
)

bin\test\shape_test.exe
if errorlevel 1 (
    echo [ERROR] shape tests failed
    popd
    exit /b 1
)

echo [OK] all tests passed
popd
