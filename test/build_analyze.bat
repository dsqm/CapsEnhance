@echo off
rem ===== MSVC static analysis pass (/analyze) =====
rem ASCII only (cmd.exe reads .bat as ANSI).
rem
rem Why this exists: /W4 only reports what the front end is sure about.
rem /analyze runs the deeper data-flow engine and finds things like
rem "buffer allocated with size X, read with size Y" and "variable mixed
rem between Interlocked and plain access" -- both classes have actually
rem occurred in this codebase (act.c stop flags, config.c line copies).
rem
rem NOTE: /analyze on this codebase is expected to produce a small number
rem of findings; the ones that remain are documented as false positives in
rem the review report. This step compiles only (no link), so it is cheap.
rem It uses the same source list as build.bat plus the unit-test sources.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\an" mkdir "obj\an"

cl /nologo /W4 /analyze /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Fo"bin\an\\" /c /I"%~dp0..\src" /I"%~dp0..\thirdparty" ^
   "%~dp0..\src\hook.c" "%~dp0..\src\nav.c" "%~dp0..\src\send.c" ^
   "%~dp0..\src\act.c" "%~dp0..\src\config.c" "%~dp0..\src\keymap.c" ^
   "%~dp0..\src\main.c" "%~dp0..\src\autodisp.c" "%~dp0..\src\bridge.c" ^
   "%~dp0..\src\wps.c" "%~dp0..\src\winmove.c" "%~dp0..\src\drag.c" ^
   "%~dp0..\src\ime.c" "%~dp0..\src\folder.c" "%~dp0..\src\help.c" ^
   "%~dp0..\src\clip.c" ^
   "%~dp0..\help\help.c" "%~dp0..\clip\clip.c" ^
if errorlevel 1 exit /b 1

echo [OK] static analysis pass finished.
exit /b 0
