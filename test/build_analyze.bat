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
rem The main-exe source list is shared with build.bat (src\sources.txt, via the
rem cl response-file syntax @): a new source file is added in ONE place. This
rem script needs the repo root as the working directory (relative paths inside
rem the response file) -- same requirement as build.bat itself.
rem The standalone exes (help / clip / settings) plus src\colorcode.c are listed
rem in full here because build.bat compiles them on their own cl lines.
rem The unit-test sources are NOT analyzed here -- they have their own build_*.bat.
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
   @src\sources.txt ^
   src\colorcode.c ^
   help\help.c clip\clip.c settings\settings.c
if errorlevel 1 exit /b 1

echo [OK] static analysis pass finished.
exit /b 0
