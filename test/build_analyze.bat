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
rem NOTE: 本仓库的基线是零发现（同一份源码的 /W4 也是零告警）。
rem 真出现发现时：要么修掉，要么把"为什么是误报"写在下面一行里 —— 别让
rem "预期会有几个告警"这类说法把新引入的问题一起盖过去。
rem This step compiles only (no link), so it is cheap.
rem It uses build.bat's source list (including settings.c). The unit-test
rem sources are NOT analyzed here -- they have their own build_*.bat.
rem NOTE: src\help.c / src\clip.c and help\help.c / clip\clip.c have the same
rem base name, and one /Fo covers the whole command -- so their .obj and
rem .nativecodeanalysis.xml overwrite each other (17 files for 19 TUs).
rem Every TU is still compiled and analyzed; only the artifacts collide.
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\an" mkdir "obj\an"

cl /nologo /W4 /analyze /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Fo"obj\an\\" /c /I"%~dp0..\src" /I"%~dp0..\thirdparty" ^
   "%~dp0..\src\hook.c" "%~dp0..\src\nav.c" "%~dp0..\src\send.c" ^
   "%~dp0..\src\act.c" "%~dp0..\src\config.c" "%~dp0..\src\keymap.c" ^
   "%~dp0..\src\main.c" "%~dp0..\src\autodisp.c" "%~dp0..\src\bridge.c" ^
   "%~dp0..\src\wps.c" "%~dp0..\src\winmove.c" "%~dp0..\src\drag.c" ^
   "%~dp0..\src\ime.c" "%~dp0..\src\folder.c" "%~dp0..\src\help.c" ^
   "%~dp0..\src\clip.c" "%~dp0..\src\colorcode.c" ^
   "%~dp0..\help\help.c" "%~dp0..\clip\clip.c" ^
   "%~dp0..\settings\settings.c"
if errorlevel 1 exit /b 1

echo [OK] static analysis pass finished.
exit /b 0
