@echo off
rem ===== build + run chord regression test =====
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin
rem test_chord.c includes main.c, which now also uses config.c and keymap.c
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Fobin\config_chord.obj src\config.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Fobin\keymap_chord.obj src\keymap.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Fobin\winmove_chord.obj src\winmove.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Fobin\test_chord.obj test\test_chord.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\test_chord.obj bin\config_chord.obj bin\keymap_chord.obj bin\winmove_chord.obj ^
   /link /OUT:bin/test_chord.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
echo [OK] compiled. running...
".\bin\test_chord.exe"
exit /b %errorlevel%