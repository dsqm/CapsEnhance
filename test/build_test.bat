@echo off
rem ===== build + run chord regression test =====
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc ^
   /Fobin\test_chord.obj test\test_chord.c ^
   /link /OUT:bin/test_chord.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib imm32.lib ^
   /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
echo [OK] compiled. running...
".\bin\test_chord.exe"
exit /b %errorlevel%