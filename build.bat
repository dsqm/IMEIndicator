@echo off
rem ===== IMEStatus portable build script (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
setlocal

call "%~dp0build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin

rem /W4: warn clean. /Brepro: reproducible build. /TP: compile as C++ so the
rem COM / UIAutomation calls (p->Method()) compile without CINTERFACE macros.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /Brepro /TP /Isrc ^
   /Fobin\ ^
   src\main.c src\config.c src\ime.c src\caret.c src\overlay.c src\debug.c ^
   /link /OUT:bin/IMEStatus.exe ^
   user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib imm32.lib comctl32.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

for %%F in (bin\IMEStatus.exe) do echo [OK] bin/IMEStatus.exe built. size=%%~zF bytes