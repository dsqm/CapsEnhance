@echo off
rem ===== CapsEnhance portable build script (MSVC x64) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
setlocal

call "%~dp0build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin

rem /W4 instead of /W3: this project has already been bitten by silent
rem sign/size mismatches in cross-process message plumbing, and the extra
rem level costs nothing. The remaining C4245s in nav.c are the OBJID_CLIENT
rem sign mismatch (harmless, kept deliberately).
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Fobin\ ^
   src\hook.c src\nav.c src\send.c src\act.c src\config.c src\keymap.c src\main.c ^
   /link /OUT:bin/CapsEnhance.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib ^
   /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] bin/CapsEnhance.exe built.
