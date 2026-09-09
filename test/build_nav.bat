@echo off
rem ===== build + run word-jump unit test =====
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "bin" mkdir bin
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc /c /Fobin\nav_test.obj src\nav.c || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc /c /Fobin\send_test.obj src\send.c || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc /c /Fobin\test_nav.obj test\test_nav.c || exit /b 1
cl /nologo /O2 /W3 /MT /utf-8 /DUNICODE /D_UNICODE /TC ^
   /Isrc /c /Fobin\test_sci.obj test\test_sci.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\test_nav.obj bin\nav_test.obj bin\send_test.obj ^
   /link /OUT:bin\test_nav.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
rem test_sci.c includes nav.c / send.c directly, so link only its own obj
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\test_sci.obj ^
   /link /OUT:bin\test_sci.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running test_nav (word-jump algorithm)...
".\bin\test_nav.exe"
set NAVRC=%errorlevel%
echo.
echo [OK] running test_sci (Scintilla channel)...
".\bin\test_sci.exe"
set SCIRC=%errorlevel%
if not "%NAVRC%"=="0" exit /b %NAVRC%
exit /b %SCIRC%