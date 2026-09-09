@echo off
rem ===== CapsEnhance portable build script (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
setlocal

call "%~dp0build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin

cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Fobin\ ^
   src\hook.c src\nav.c src\send.c src\main.c ^
   /link /OUT:bin/CapsEnhance.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] bin/CapsEnhance.exe built.
