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

cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\nav_wps.obj      src\nav.c       || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\send_wps.obj     src\send.c     || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\cfg_wps.obj      src\config.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\km_wps.obj       src\keymap.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\wps_wps.obj      src\wps.c      || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\autodisp_wps.obj src\autodisp.c || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\bridge_wps.obj   src\bridge.c   || exit /b 1
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /TP /Isrc /c /Foobj\wpsjump.obj      test\wpsjump.c || exit /b 1
cl /nologo /MT /utf-8 /DUNICODE /D_UNICODE ^
   obj\wpsjump.obj obj\nav_wps.obj obj\send_wps.obj obj\cfg_wps.obj obj\km_wps.obj ^
   obj\wps_wps.obj obj\autodisp_wps.obj obj\bridge_wps.obj ^
   /link /OUT:bin\wpsjump.exe ^
   user32.lib gdi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib ^
   /SUBSYSTEM:CONSOLE || exit /b 1
echo [OK] compiled. running...
".\bin\wpsjump.exe"
exit /b %errorlevel%
