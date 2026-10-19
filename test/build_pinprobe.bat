@echo off
rem ===== window-pinning acceptance probe =====
rem Builds two exes under obj\test\:
rem   pintarget.exe  a plain window to pin (the probe cannot pin its own process)
rem   pinprobe.exe   pins it, then measures geometry, edge pixels and per-frame lag
rem Run from the repo root:  obj\test\pinprobe.exe
rem
rem Why a separate probe: the border lives on another thread and really moves system
rem windows, so the chord test (test_chord.c) can only assert the pure helpers.
rem This one measures what a user feels: is the frame where it should be, is the edge
rem the accent colour, and how much does pinning cost per move.
setlocal
call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\test" mkdir obj\test

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Foobj\ /Feobj\test\pintarget.exe test\pintarget.c ^
   /link user32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1

rem /std:c++17: src\pin.cpp is a straight port of PowerToys' AlwaysOnTop.
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /std:c++17 /Isrc ^
   /Foobj\ /Feobj\test\pinprobe.exe test\pinprobe.c src\pin.cpp ^
   /link user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib winmm.lib d2d1.lib dwmapi.lib ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

echo [OK] obj/test/pintarget.exe, obj/test/pinprobe.exe
