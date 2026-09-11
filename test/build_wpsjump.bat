@echo off
rem ===== build + run the WPS channel live test (needs WPS running with a doc) =====
rem ASCII only: cmd.exe reads .bat as ANSI.
rem This one links the REAL channel modules -- nav(dispatch) + wps(object model) +
rem autodisp(IDispatch by name) + bridge(lowered-integrity IPC) -- so the test
rem exercises the shipped code path, not a stub.
setlocal
call "%~dp0..\build_env.bat"
if errorlevel 1 exit /b 1
if not exist "bin" mkdir bin

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\nav_wps.obj      src\nav.c       || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\send_wps.obj     src\send.c     || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\cfg_wps.obj      src\config.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\km_wps.obj       src\keymap.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\wps_wps.obj      src\wps.c      || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\autodisp_wps.obj src\autodisp.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\bridge_wps.obj   src\bridge.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TC /Isrc /c /Fobin\wpsjump.obj      test\wpsjump.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   bin\wpsjump.obj bin\nav_wps.obj bin\send_wps.obj bin\cfg_wps.obj bin\km_wps.obj ^
   bin\wps_wps.obj bin\autodisp_wps.obj bin\bridge_wps.obj ^
   /link /OUT:bin\wpsjump.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running...
".\bin\wpsjump.exe"
exit /b %errorlevel%
