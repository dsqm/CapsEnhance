@echo off
rem ===== build + run the WPS channel live test (needs WPS running with a doc) =====
setlocal
call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1
if not exist "bin" mkdir bin

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\nav_wps.obj  src\nav.c    || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\send_wps.obj src\send.c  || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\cfg_wps.obj  src\config.c|| exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\km_wps.obj   src\keymap.c|| exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\wpsjump.obj test\wpsjump.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\wpsjump.obj bin\nav_wps.obj bin\send_wps.obj bin\cfg_wps.obj bin\km_wps.obj ^
   /link /OUT:bin\wpsjump.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running...
".\bin\wpsjump.exe"
exit /b %errorlevel%
