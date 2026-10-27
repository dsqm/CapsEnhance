@echo off
rem ===== build + run word-jump unit test =====
setlocal

call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1

if not exist "obj\test" mkdir obj\test
if not exist "obj" mkdir obj
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\nav_test.obj src\nav.c || exit /b 1
rem nav_word.c: the pure word-boundary algorithm nav.c links against
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\navword_test.obj src\nav_word.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\send_test.obj src\send.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\config_test.obj src\config.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\keymap_test.obj src\keymap.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\test_nav.obj test\test_nav.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\test_sci.obj test\test_sci.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\test_config.obj test\test_config.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP ^
   /Isrc /c /Foobj\test_child.obj test\test_child.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   obj\test_nav.obj obj\nav_test.obj obj\navword_test.obj obj\send_test.obj ^
   obj\config_test.obj ^
   /link /OUT:obj\test\test_nav.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
rem test_sci.c includes nav.c / send.c / config.c directly, so link only its own obj
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   obj\test_sci.obj obj\config_test.obj obj\navword_test.obj ^
   /link /OUT:obj\test\test_sci.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   obj\test_config.obj obj\config_test.obj obj\keymap_test.obj ^
   /link /OUT:obj\test\test_config.exe user32.lib advapi32.lib /SUBSYSTEM:CONSOLE || exit /b 1
rem test_child.c includes src\child.c directly, so link only its own obj
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   obj\test_child.obj ^
   /link /OUT:obj\test\test_child.exe user32.lib /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running test_nav (word-jump algorithm)...
".\obj\test\test_nav.exe"
set NAVRC=%errorlevel%
echo.
echo [OK] running test_sci (Scintilla channel)...
".\obj\test\test_sci.exe"
set SCIRC=%errorlevel%
echo.
echo [OK] running test_config (ini parsing)...
".\obj\test\test_config.exe"
set CFGRC=%errorlevel%
echo.
echo [OK] running test_child (subprocess lifetime: job + death notify)...
".\obj\test\test_child.exe"
set CHILDRC=%errorlevel%
if not "%NAVRC%"=="0" exit /b %NAVRC%
if not "%SCIRC%"=="0" exit /b %SCIRC%
if not "%CFGRC%"=="0" exit /b %CFGRC%
exit /b %CHILDRC%
