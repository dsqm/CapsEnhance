@echo off
rem ===== build wheelprobe.exe (wheel landing counter, MSVC x64 console) =====
rem ASCII only: cmd.exe reads .bat as ANSI, non-ASCII comments break parsing.
rem Standalone: depends on nothing in src\ (pure Win32 + CRT).
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\test" mkdir obj\test

rem Output goes to obj\test\ on purpose: bin\ holds ONLY the four release exes
rem (see docs\DEVELOPMENT.md). Diagnostic tools live in obj\test\ so that
rem "publish = take bin\" never ships a probe.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Foobj\test\ /Fdobj\test\ ^
   test\wheelprobe.c ^
   /link /OUT:obj\test\wheelprobe.exe user32.lib gdi32.lib /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

echo [OK] obj\test\wheelprobe.exe built.
exit /b 0
:fail
echo [ERROR] build failed
exit /b 1
