@echo off
rem ===== IMEIndicator tests (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
rem
rem Config tests run inside obj\test\ so CfgSave/CfgLoad (which use the exe's
rem own directory) never touch the real IMEIndicator.json next to the main program.
setlocal
pushd "%~dp0"

call "build_env.bat"
if errorlevel 1 (
    popd
    exit /b 1
)

if not exist "obj\test" mkdir "obj\test"

rem ---- config layer: JSON round-trip, defaults, inherit, migration, blacklist ----
rem config.c calls DbgLog on the ini-migration failure path -> link debug.c too (else LNK2019)
call :run_test config_test "tests\config_test.c src\config.c src\json.c src\debug.c" "user32.lib shell32.lib advapi32.lib"
if errorlevel 1 goto :fail

rem ---- shape rasterizer: area, antialiasing, premultiplied alpha ----
call :run_test shape_test "tests\shape_test.c src\shape.c" ""
if errorlevel 1 goto :fail

rem ---- settings UI painting: coverage AA, premultiplied layer, AlphaBlend ----
call :run_test uipaint_test "tests\uipaint_test.c src\uipaint.c" "user32.lib gdi32.lib msimg32.lib"
if errorlevel 1 goto :fail

rem ---- image shapes: WIC decode, aspect scaling, import dedup ----
rem links config.c as well -> needs debug.c for DbgLog
call :run_test image_test "tests\image_test.c src\image.c src\config.c src\json.c src\debug.c" "user32.lib shell32.lib advapi32.lib ole32.lib windowscodecs.lib"
if errorlevel 1 goto :fail

rem ---- settings dirty check: whether the Save button lights up ----
rem uses CfgDefaults -> link config.c (and its json/debug deps)
call :run_test settings_dirty_test "tests\settings_dirty_test.c src\settings_dirty.c src\config.c src\json.c src\debug.c" "user32.lib shell32.lib advapi32.lib"
if errorlevel 1 goto :fail

echo [OK] all tests passed
popd
exit /b 0

:fail
popd
exit /b 1

rem ---- build + run one test exe ----
rem %1 = test name (exe base), %2 = source list, %3 = lib list (may be empty)
:run_test
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Foobj\test\ ^
   %~2 ^
   /link /OUT:obj\test\%~1.exe %~3 ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] %~1 build failed
    exit /b 1
)

obj\test\%~1.exe
if errorlevel 1 (
    echo [ERROR] %~1 tests failed
    exit /b 1
)
exit /b 0
